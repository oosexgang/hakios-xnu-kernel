/*
 * libhakios.store.dylib: code of ours inside appstored (see inject.c for how it gets there).
 *
 * What it is for. The App Store opens and shows LOADING for ever. The front page is a "bag" --
 * AppleMediaServices' configuration -- and AMS will not load one until it knows the storefront, the
 * number that says which country's store this device belongs to. Everything outside the process has
 * been measured and is well:
 *   - the bag itself is fetched from this phone by hand, 56 KB in half a second, and the server
 *     hands over a storefront in the bargain: "x-set-apple-store-front: 143469-2,8" from both
 *     bag.itunes.apple.com and init.itunes.apple.com (rc.jobs.store, src/net_test.m);
 *   - appstored opens no socket at all while the store spins, so nothing is failing on the network;
 *   - the storefront cannot be put on the device from another process: AMS's "local" account is not
 *     a row in accountsd at all -- a different identifier every boot -- so there is nothing to write
 *     to and nothing that would keep (src/store_test.m).
 *
 * So the storefront is not written anywhere here either. The account is made to ANSWER with one:
 * -[ACAccount ams_storefront] is the door every consumer in AMS goes through, and its neighbours are
 * taken in the same pass because different callers knock at different ones. No persistence, no
 * accountsd, nothing that outlives the process.
 *
 * And the question that is worth more than the storefront: is a bag request ever built? From outside
 * only the socket log could answer, and only negatively, over a console that drops lines. From in
 * here it can be answered positively. -[AMSBagNetworkTask _performFetchWithAttemptedCount:account:
 * storefront:] and its _queryItemsForClientInfo:... are where a request is assembled: they are not
 * replaced, only watched, and what they were called with is written down. If neither is entered,
 * nothing about the network matters and the gate is above them.
 *
 * The storefront is 143469-2,8 because that is what Apple's own bag server returned for THIS device
 * when asked with none -- a region, picked from the address the request came from. No Apple ID is
 * signed in, none can be, and the store must still refuse to install: there is nothing to buy with.
 *
 * AppleMediaServices and Accounts are not loaded yet when this library is: the methods are taken
 * when their images arrive (_dyld_register_func_for_add_image).
 *
 * WHAT IT ANSWERED. The storefront was the gate, and it is not the only one.
 *   entered    3 -[ACAccount ams_storefront]                     (answered 143469-2,8; was (nil))
 *   entered    3 -[AMSBagActiveAccountProvider _bagAccountForAccountMediaType:mustContainStorefront:]
 *                 mustContainStorefront:0 -> the local account, mustContainStorefront:1 -> (nil)
 *   entered    1 -[AMSBagNetworkTask _performFetchWithAttemptedCount:account:storefront:]
 *                 "BAG FETCH attempt 0, storefront 143469-2,8, account local (...)" -> a promise
 *   entered    1 -[AMSBagNetworkTask _createRequestWithQueryItems:] -> <AMSMutablePromise: ...>
 *   entered    0 -[AMSBagNetworkTask URLSessionPromise]
 *   entered    0 -[AMSURLSession dataTaskPromiseWithRequest:]
 *   entered    0 -[AMSURLSession dataTaskWithRequest:]
 *   entered    0 -[NSURLSessionTask resume]
 * So with a storefront to answer with, AMS DOES build a bag request -- that question is settled, and
 * positively, from inside. And the request never becomes a transfer: it is assembled into an
 * AMSMutablePromise that nothing ever resolves, so no session is asked for, no task is made and
 * nothing is ever resumed. Not one task is resumed in the whole daemon in a six-minute session,
 * which is why the socket log never had appstored in it. The store still shows LOADING, as it must:
 * the bag never loads.
 * AND THEN, with entry and exit counted separately -- a method that hangs and a method that is
 * never called leave the same silence below them, so every wrapper counts both:
 *   entered    1 returned    1 -[AMSBagNetworkTask _createRequestWithQueryItems:]
 *   entered    0 returned    0  the four steps that finish a request (cookies, storefront header,
 *                               anisette headers, -[AMSURLSession _prepareRequest:properties:error:])
 *   entered   12 returned   12 -[AMSMutablePromise continueWithBlock:]
 *   entered    5 returned    5 -[AMSMutablePromise thenWithBlock:]
 *   entered    1 returned    1 -[AMSMutablePromise _finishWithResult:error:logDuplicateFinishes:]
 * Nothing hangs. _createRequestWithQueryItems: returns at once, with a promise -- and that promise is
 * never finished. Seventeen continuations are attached to promises in that session and exactly ONE
 * promise is ever finished, and by address it is not the request's:
 *   PROMISE 0x101709240 waited on (continue)
 *   STEP _createRequestWithQueryItems: -> <AMSMutablePromise: 0x10170b930>
 *   PROMISE 0x10170b930 waited on (then) / (continue)
 *   PROMISE 0x101414370 finished          <- the only one, and a different promise
 * So the bag request is never produced at all: the step that would produce it is waiting on
 * something that never arrives, which is why no header is ever added, no session asked for, no task
 * made and nothing resumed. The anisette guess is ruled out with the other three: none is entered.
 *
 * AND THE THREADS SAY WHY, which is a bigger thing than the bag. All eleven threads of the daemon
 * are waiting (run state 3; one idle libdispatch worker in __workq_kernreturn, so the queues are not
 * starved of threads), and four of them are in the same place:
 *   -[AMSBagValue valueWithError:] -> -[AMSPromise resultWithError:] -> resultBeforeDate:error:
 *   -> -[NSConditionLock lockWhenCondition:beforeDate:] -> -[NSCondition waitUntilDate:]
 * -- a thread blocked until the bag loads. Walking every thread's frames and giving them to
 * "ipsw dyld a2s" names the circle:
 *   -[AMSBagNetworkTask performFetch]_block_invoke
 *     -> -[AMSBagActiveAccountProvider bagStorefrontForAccountMediaType:]
 *       -> _bagAccountForAccountMediaType:mustContainStorefront:
 *         -> -[ACAccountStore _ams_localiTunesAccountAfterUpdatingStorefront:]
 *           -> -[AMSPromise resultWithError:], for ever
 * while the promise it waits on is finished by the continuation of
 * _fetchLocaliTunesAccountForAccountType:shouldUpdateStorefront: running
 * _updateStorefrontInLocalAccount:, which is blocked in resultWithError: on another thread. The
 * thread that would fetch the bag waits on an account update that waits on the bag.
 * (The same walk explains an earlier puzzle: the cookies are added by the FOUR-argument
 * ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies:, inside
 * -[AMSURLRequestEncoder requestByEncodingRequest:parameters:], which is why a hook on the
 * three-argument one was never entered.)
 * Answering _ams_localiTunesAccountAfterUpdatingStorefront: at once (above) does cut that wait --
 * _bagAccountForAccountMediaType: goes from blocked to 3 entered 3 returned -- and the bag still
 * does not load: four threads are still blocked in resultBeforeDate:error: elsewhere. So the stall
 * is not one method; AMS's promises in this daemon are not being completed at all.
 *
 * THE ROOT, found by the promise graph rather than by recognising another circle. Every promise is
 * remembered here by address, with where it was first seen, how many continuations were attached,
 * which threads block in it and whether it was ever finished; the table is printed at 150 s beside
 * the thread dump, and a promise with waiters and no finish is marked. Sixteen promises in a
 * session, seven of them so marked, the bag's request and the bag's fetch among them. Following the
 * threads of the marked ones down (ipsw dyld a2s again) ends outside AMS altogether:
 *   thread 4: mach_msg2_trap <- _AMSFairPlayGetDeviceType <- _CjHbHx <- (three FairPlay functions)
 * -- an AMS thread blocked in a Mach call into FairPlay, waiting for a reply. And in the same
 * session's launchd log:
 *   com.apple.fairplayd.H2: 34 spawns, every one "Service only ran for 0 seconds",
 *   "exited due to exit(0)", "service throttled by 10 seconds"
 * fairplayd does not crash: it starts, decides there is nothing it can do here, and leaves at once,
 * over and over. So the reply AMS is waiting for never comes, the promise behind it is never
 * fulfilled, and seventeen waits queue behind that one. The storefront deadlock sits on top of this,
 * not beside it.
 * The next piece of work is therefore not in AMS at all: find why fairplayd.H2 exits 0 immediately
 * and keep it up. Everything in this file above that line is a consequence of it.
 *
 * Two things this library learned about itself, for whoever writes the next one:
 *   - class_getInstanceMethod walks up the chain, so replacing what a class only INHERITS replaces it
 *     for everything else that inherits it too. Doing that to an NSObject category restarted
 *     appstored 31 times in one run. image_added now refuses anything the superclass answers to.
 *   - an ObjC exception ends in abort(), not in SIGSEGV, so a fault reporter that watches only the
 *     memory signals says nothing at all about the usual way a replacement of ours kills a daemon.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "absinthe_bag.h"

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */
/*
 * The storefront answered for an account that has none. 16.5 used 143469-2,8; on iOS 26/27 the App Store's
 * own bag fetch carries 143469-2,29 (its "apps3" media type), which is also the one the working catalog of
 * 16.5 was recorded with, so the suffix is matched to what this store asks with rather than left at 8.
 */
#define STOREFRONT "143469-2,29"

static uintptr_t slide;
static bool watch_only;         /* accountsd: looked at, never changed (see the constructor) */
static bool g_absinthe_only;    /* amsaccountsd: install only the ISAMSBagShim absinthe-requests row */
static CFStringRef storefront;

/* the table itself is at the end, where its functions are; the fetch hook reports what is missing */
struct replacement {
	const char *class_name;
	const char *selector;
	bool class_method;
	IMP replacement;
	IMP *original;
	bool done;
};
static struct replacement replacements[];
static size_t replacement_count(void);
static void image_added(const struct mach_header *header, intptr_t image_slide);
static void make_account_types(void);
static const char *knob(const char *name);     /* a switch from /var/hakios-store.conf; see knob() */
static const char *who_i_am(void);              /* this process name; defined below */
static void dump_tracked_promises(void);        /* defined with the bootstrap hooks */
static void force_resolve_prereqs(void);        /* defined with the bootstrap hooks */
static void dump_prereq_types(void);            /* defined with the bootstrap hooks */
static void dump_tracked_conts(void);           /* defined with the bootstrap hooks */
static void install_cont_spy(void);             /* deferred continuation hooks, put in at start() */

/*
 * How many times each hook was entered: a count that survives a console that drops lines. The names
 * below are in the order the hooks count themselves in, which is NOT the order of the table at the
 * end -- the first version of this printed one against the other and every count came out against
 * the wrong name, which had to be undone by hand afterwards. Keep the two lists apart on purpose.
 */
enum {
	COUNT_ams_storefront, COUNT_cachedStorefront, COUNT_bagStorefront_fixed, COUNT_bagStorefront_active,
	COUNT_copyStorefront, COUNT_bagAccount, COUNT_performFetch, COUNT_queryItems, COUNT_performRequest,
	COUNT_performRequestBlock, COUNT_resume, COUNT_resumeLocal, COUNT_createRequest, COUNT_sessionPromise,
	COUNT_dataTaskPromise, COUNT_dataTask, COUNT_addCookies, COUNT_addStorefrontHeader, COUNT_addAnisette,
	COUNT_prepareRequest, COUNT_finishWithResult, COUNT_finishWithError, COUNT_finishFunnel,
	COUNT_finishResultError, COUNT_thenWithBlock, COUNT_continueWithBlock, COUNT_bagValue,
	COUNT_resultBefore, COUNT_afterUpdating, COUNT_addCookies4, COUNT_fpDeviceType, COUNT_brotli,
	COUNT_processStream, COUNT_updateStorefront, COUNT_addCookiesBlocking, COUNT_dataTaskCreated,
	COUNT_storefrontMedia,
	COUNT_last
};
static const char *const counted[COUNT_last] = {
	"-[ACAccount ams_storefront]", "-[AMSBagNetworkDataSource cachedStorefront]",
	"-[AMSBagFixedAccountProvider bagStorefrontForAccountMediaType:]",
	"-[AMSBagActiveAccountProvider bagStorefrontForAccountMediaType:]",
	"-[ACAccount ams_copyStorefrontFromAccount:]",
	"-[AMSBagActiveAccountProvider _bagAccountForAccountMediaType:mustContainStorefront:]",
	"-[AMSBagNetworkTask _performFetchWithAttemptedCount:account:storefront:]",
	"-[AMSBagNetworkTask _queryItemsForClientInfo:...]", "-[AMSURLSession performRequest:completion:]",
	"-[AMSURLSession performRequest:completionBlock:]", "-[NSURLSessionTask resume]",
	"-[__NSCFLocalSessionTask resume]", "-[AMSBagNetworkTask _createRequestWithQueryItems:]",
	"-[AMSBagNetworkTask URLSessionPromise]", "-[AMSURLSession dataTaskPromiseWithRequest:]",
	"-[AMSURLSession dataTaskWithRequest:]", "ams_addCookiesForAccount:clientInfo:bag:",
	"ams_addStoreFrontHeaderForAccount:client:withBag:", "ams_addAnisetteHeadersForAccount:type:bag:",
	"-[AMSURLSession _prepareRequest:properties:error:]", "-[AMSMutablePromise finishWithResult:]",
	"-[AMSMutablePromise finishWithError:]", "-[AMSMutablePromise _finishWithResult:error:logDuplicateFinishes:]",
	"-[AMSMutablePromise finishWithResult:error:]", "-[AMSMutablePromise thenWithBlock:]",
	"-[AMSMutablePromise continueWithBlock:]", "-[AMSBagValue valueWithError:]",
	"-[AMSPromise resultBeforeDate:error:]",
	"-[AMSBagValue valueWithError:]", "-[AMSPromise resultBeforeDate:error:]",
	"-[ACAccountStore _ams_localiTunesAccountAfterUpdatingStorefront:]",
	"ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies:",
	"-[AMSUserAgent _sharedComponentFairPlayDeviceType]",
	"-[JEBrotliDecoder decompressStreamWithAvailableIn:nextIn:availableOut:nextOut:]",
	"-[JPReader processStream:signatureVerifier:options:error:]",
	"-[ACAccountStore _updateStorefrontInLocalAccount:]",
	"ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies: (answered empty)",
	"-[NSURLSession dataTaskWithRequest:...] (created, not resumed)",
	"-[ACAccount ams_storefrontForMediaType:]",
};
static unsigned long entered[COUNT_last];
/*
 * And how many times each came BACK. A method that hangs and a method that is never called look the
 * same in one counter -- both leave everything below them at zero -- so every wrapper counts its
 * entry and its return separately. The one with entered > returned is where the thread is.
 */
static unsigned long returned[COUNT_last];
static unsigned serial;


/* Each line of ours alone on the console: it drops them under the start's flood (see rc.jobs.store). */
/*
 * To the console itself, not to standard error. A launchd job of ours has /dev/console on fd 2
 * (mkjobs.py sets StandardErrorPath), but an application started by SpringBoard has nothing there,
 * so the first run with this library inside AppStore.app looked exactly like a run where it had not
 * been inserted at all -- another silence that meant "wrong channel", not "did not happen".
 */
static const char *who = "store";

/* Best-effort copy of a log line to the host line sink (192.168.9.80:8100), eviction-proof. */
static void
sink_send(const char *buf, int n)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr;
	struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };

	if (s < 0 || n <= 0) {
		if (s >= 0) { close(s); }
		return;
	}
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(8100);
	addr.sin_addr.s_addr = inet_addr("192.168.1.18");
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
		write(s, buf, (size_t)n);
	}
	close(s);
}

static void
say(const char *format, ...)
{
	static int console = -2;
	char line[512];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (console == -2) {
		console = open("/dev/console", O_WRONLY | O_APPEND);
	}
	/*
	 * And say which process. Two of them carry this library and both write to the one console, so a
	 * report read by position belongs to whichever process happened to be writing -- the lines are
	 * interleaved, not grouped. Every line names its own program instead.
	 */
	if (strncmp(line, "hakios: store: ", 15) == 0) {
		char named[sizeof(line) + 32];

		/*
		 * 15, the length of the prefix -- not 16. Comparing 16 bytes compared the literal's own NUL
		 * against the first character of the rest, which never matches, so no line was ever renamed:
		 * every line said "store" and the two processes' reports read as one. That is what made the
		 * daemon look as if none of its hooks ran.
		 */
		n = snprintf(named, sizeof(named), "hakios: %s: %s", who, line + 15);
		if (n > (int)sizeof(named) - 1) {
			n = sizeof(named) - 1;
		}
		write(console >= 0 ? console : 2, named, (size_t)n);
		if (strstr(named, "error") || strstr(named, "COMPLETED") || strstr(named, "ccount")) {
			sink_send(named, n);
		}
		return;
	}
	write(console >= 0 ? console : 2, line, (size_t)n);
	if (strstr(line, "error") || strstr(line, "COMPLETED") || strstr(line, "ccount")) {
		sink_send(line, n);
	}
}

/* an object as a short string, for the log: a description, cut to fit */
static const char *
text_of(id object, char *buffer, size_t size)
{
	CFStringRef description;

	buffer[0] = 0;
	if (object == NULL) {
		return "(nil)";
	}
	description = (CFStringRef)((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("description"));
	if (description == NULL || !CFStringGetCString(description, buffer, (CFIndex)size, kCFStringEncodingUTF8)) {
		return "(?)";
	}
	return buffer;
}

static unsigned
step_in(int which)
{
	unsigned n = __sync_add_and_fetch(&serial, 1);

	entered[which]++;
	say("hakios: store: -> #%u %s\n", n, counted[which]);
	return n;
}

static void
step_out(int which, unsigned n, bool got_something)
{
	returned[which]++;
	say("hakios: store: <- #%u %s%s\n", n, counted[which], got_something ? "" : " (nil)");
}

/*
 * The fault reporter, as in inject.c: iOS answers a crash in a stock daemon with
 * "Failed to send exception EXC_CORPSE_NOTIFY" and nothing else, so a fault in here would be a
 * silent exit. This turns one into an address, unslid, which "ipsw dyld a2s" turns into a name.
 */
/*
 * Up the frame pointers: on arm64 each frame holds the previous one at [fp] and its return address
 * at [fp + 8]. Unslid, so that "ipsw dyld a2s" turns each into a name.
 */
static void
frames_from(uintptr_t fp, char *text, size_t size)
{
	size_t used = 0;

	text[0] = 0;
	for (unsigned i = 0; i < 10 && fp != 0 && (fp & 7) == 0 && used + 20 < size; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(text + used, size - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;      /* the chain only ever goes up the stack */
	}
}

/*
 * WHAT CAME BACK, not that a task was started. "It fetches its content and the screen still says
 * LOADING" has four shapes behind it -- the request never completes, it completes with an HTTP
 * error, it completes with a body the renderer rejects, or it completes and nothing is drawn -- and
 * they are the same line without the response. So every task that is resumed is kept (retained, or
 * it would be gone by the time the report runs) and asked at 150 s what became of it: its state,
 * the status, the bytes received, the type, and the error if there is one.
 */
#define TASKS 64
static id tasks[TASKS];
static unsigned task_count;
static pthread_mutex_t task_lock = PTHREAD_MUTEX_INITIALIZER;

static void
remember_task(id task)
{
	pthread_mutex_lock(&task_lock);
	if (task_count < TASKS) {
		tasks[task_count++] = (id)CFRetain((CFTypeRef)task);
	}
	pthread_mutex_unlock(&task_lock);
}

/*
 * For the storefront token request (sf-api-token-service/apiToken) the one thing that decides
 * local-versus-server is whether an Authorization -- or any of the anisette X-Apple-* fields -- is
 * attached to what went out. Empty means AppSSO made nothing (the -1000 beside every 401) and the
 * request never carried our credentials, so the server's 401 is not a judgement on them and the
 * block is this side of the wire; present means the server saw an assertion and refused it, which is
 * the Secure-Enclave attestation wall that also stops activation. currentRequest is read, not
 * originalRequest, because the account and anisette fields are added by the AMS URL-protocol handler
 * after the task is made. Only names and value LENGTHS are logged, never a token, because this
 * file's output is kept.
 */
static void
dump_token_headers(id task, const char *which)
{
	id request = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("currentRequest"));
	id headers, keys;
	long n;

	if (request == NULL) {
		request = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("originalRequest"));
	}
	headers = request != NULL ? ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("allHTTPHeaderFields")) : NULL;
	keys = headers != NULL ? ((id (*)(id, SEL))objc_msgSend)(headers, sel_registerName("allKeys")) : NULL;
	n = keys != NULL ? ((long (*)(id, SEL))objc_msgSend)(keys, sel_registerName("count")) : 0;
	/*
	 * Named by its own URL. This dump is asked for the token request AND for the bag's, and saying
	 * "apiToken" for both made the bag's headers read as the token's: the token request looked as
	 * though it went out bare, when on iOS 27 it is never made at all.
	 */
	say("hakios: store: %.90s carries %ld header(s):\n", which, n);
	for (long i = 0; i < n; i++) {
		id key = ((id (*)(id, SEL, long))objc_msgSend)(keys, sel_registerName("objectAtIndex:"), i);
		id val = ((id (*)(id, SEL, id))objc_msgSend)(headers, sel_registerName("objectForKey:"), key);
		long len = val != NULL ? ((long (*)(id, SEL))objc_msgSend)(val, sel_registerName("length")) : -1;
		char kb[128];

		say("hakios: store:   %s = (%ld chars)\n", text_of(key, kb, sizeof(kb)), len);
	}
}

static void
dump_tasks(void)
{
	static const char *const states[] = { "running", "suspended", "cancelling", "COMPLETED" };

	say("hakios: store: %u tasks resumed\n", task_count);
	for (unsigned i = 0; i < task_count; i++) {
		id task = tasks[i];
		long state = ((long (*)(id, SEL))objc_msgSend)(task, sel_registerName("state"));
		id response = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("response"));
		id error = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("error"));
		id request = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("originalRequest"));
		id url = request != NULL ? ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("URL")) : NULL;
		long long got = ((long long (*)(id, SEL))objc_msgSend)(task, sel_registerName("countOfBytesReceived"));
		long status = response != NULL ? ((long (*)(id, SEL))objc_msgSend)(response, sel_registerName("statusCode")) : 0;
		id type = response != NULL ? ((id (*)(id, SEL))objc_msgSend)(response, sel_registerName("MIMEType")) : NULL;
		char one[96], two[64], three[200];

		say("hakios: store: task %u %s status %ld, %lld bytes, %s%s%s, %s\n", i,
		    state >= 0 && state < 4 ? states[state] : "?", status, got,
		    type != NULL ? text_of(type, one, sizeof(one)) : "(no type)",
		    error != NULL ? ", error " : "", error != NULL ? text_of(error, two, sizeof(two)) : "",
		    url != NULL ? text_of(url, three, sizeof(three)) : "(no url)");
		if (url != NULL && (strstr(three, "apiToken") != NULL || strstr(three, "bag.itunes") != NULL)) {
			dump_token_headers(task, strstr(three, "apiToken") != NULL ? "the apiToken request" :
			    "the bag request");
		}
	}
}

/*
 * WHAT HAPPENS TO THE PAGE AFTER IT ARRIVES. The front page comes down whole and nothing draws it,
 * and the report that showed that also showed something nobody was looking for: THE SAME 177 786
 * BYTES WERE FETCHED TWICE. A document that parsed would not be fetched again, so the likeliest
 * story is loaded, rejected, tried again.
 * JetEngine says how it would reject one. It carries jetpackSignatureVerificationDidFailNotification
 * -- a signature is verified and there is a named way of announcing that it was not liked -- and a
 * JEBrotliDecoder, so the body is compressed before any of that; JetPackSigningPolicy,
 * JetpackSignatureVerifierP, JetPackageFormatParser and JetpackErrorDomain are all in the same
 * library. Every one of those rejects AFTER the bytes are on disk, which is the shape measured.
 * Rather than guess the notification's string (a name is one of the ways an instrument lies -- see
 * below), every notification posted in the process is watched and the ones whose name speaks of
 * jetpacks, signatures, verification or brotli are printed.
 * WHAT IT SAYS. No signature-failure notification is ever posted: 169 to 197 notifications in a
 * session of the app and not one about a jetpack, a signature, verification or brotli -- the only
 * JetEngine notification seen at all is JetEngine.Unstable.Accounts.didChangeNotification, so the
 * engine is alive and posting.
 * AND THE READER IS PLAINLY HAPPY. With the counters printed on their own line, early, instead of as
 * one line of a sixty-line report:
 *   hakios: AppStore: JETPACK processStream: in
 *   hakios: AppStore: JETPACK processStream: out, a result, error (none)
 *   hakios: AppStore: KEY brotli entered 0 returned 0, processStream entered 2 returned 2
 *   hakios: appstored: KEY brotli entered 0 returned 0, processStream entered 0 returned 0
 * -[JPReader processStream:signatureVerifier:options:error:] is entered twice -- once per fetch --
 * and both times it RETURNS A RESULT WITH NO ERROR. So the page is fetched whole and parsed without
 * complaint: it is not the signature, not the package format, and not Brotli (which never runs at
 * all). That retires all three at once, and with them the experiment of swapping
 * +[JPSignatureVerifier defaultSignatureVerifier] for +unsafeIgnoreSignatureVerifier -- there is
 * nothing for it to prove, since the verification already passes. An image that does not verify
 * signatures is a worse thing than an experiment that does not, and this one is not needed.
 * The only C++ exception thrown anywhere in either process is std::future_error (every throw is
 * watched through __cxa_throw, whose type name is the second word of its std::type_info), and it is
 * thrown in appstored as well, where everything now works -- so it looked like a lead and not an
 * answer. COUNTED, it looked less innocent: 95 of them in appstored and 68 in the App Store in one
 * session, with only 4 and 2 NSExceptions beside them, and "seventeen waits and one finish" is the
 * shape a flood of broken promises would make. It was tempting, and it is WRONG.
 * The frames and the times say so, which is why both are printed:
 *   C++ THROW #1 at 0.11 s NSt3__112future_errorE from 0x18ff0bd14 0x18fd1c8b8 0x18fd197f0 ...
 *   0x18ff0bd14: std::make_exception_ptr<std::future_error>
 *   0x18fd1c8b8: std::promise<std::tuple<PromiseResult>>::~promise
 *   0x18fd197f0: std::__shared_ptr_emplace<AMSCore::Future<PromiseResult>::FutureImp>::__on_zero_shared
 *   0x1900b57dc: std::shared_ptr<AMSCore::Future<PromiseResult>::FutureImp>::~shared_ptr
 *   0x18fd1a9c4: -[AMSPromise .cxx_destruct]
 * Every one of the first throws is the same path, and every one is inside the first quarter-second
 * of the process -- 0.11 to 0.26 s, long before anything is fetched. It is an AMSPromise being
 * DEALLOCATED: ~promise stores a broken_promise into the shared state when it is destroyed without
 * a value, which is what the C++ standard says it must do, and make_exception_ptr is how it is
 * stored rather than how it is reported. 163 of them a session is AMS's style of letting go of
 * promises nobody needs, not a flood of failures. The number means nothing on its own, the
 * connection to "nothing asks for content" is disproved, and this paragraph is here so that the
 * next reader does not find the number again and make the same handsome mistake.
 * WHAT IS LEFT is the drawing itself: 177 786 bytes arrive, JPReader parses them into a result, and
 * nothing appears. Whoever owns it next is on the stack when it is handed back, so the walk is done
 * at the exit too, and "ipsw dyld a2s" names the image if not the function:
 *   JETPACK processStream: out, a result, error (none), to 0x19d96245c 0x19d963f88 0x19d9634bc ...
 *   0x19d96245c: func_19d962020 + 1084
 *     dylib=/System/Library/PrivateFrameworks/JetEngine.framework/JetEngine
 * -- straight back into JetEngine, into Swift functions the cache carries no names for. That is
 * where the next session starts, and it is static work: the project's Ghidra path (see
 * src/inject_fairplay.c for the tools and their rules).
 * AND THE APP'S OWN WAITS ARE GONE, which is what justifies going there. The graph had shown the app
 * NOT all finished: 192 promises, two threads each parked on two that nobody fulfils -- one in
 * -[ACAccountStore _updateStorefrontInLocalAccount:], one in
 * ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies: inside
 * -[AMSURLRequestEncoder requestByEncodingRequest:parameters:] -- the same family as the deadlock
 * cut in appstored, left over because the hook that cut that one only answers
 * _ams_localiTunesAccountAfterUpdatingStorefront:, and -[AMSBagValue valueWithError:] standing at 92
 * entered and 91 returned, one outstanding at the moment the report printed. Both are answered now
 * (above), and the next run says:
 *   entered   89 returned   89 -[AMSBagValue valueWithError:]
 *   entered  104 returned  104 -[AMSPromise resultBeforeDate:error:]
 *   and not one "STILL IN IT" anywhere in the app
 * Every wait completes, the page is still fetched and still parses without error -- and the screen
 * still says LOADING. So the drawing is not waiting on AppleMediaServices for anything: the last
 * suspects outside JetEngine are cleared, and the static read of its Swift is justified rather than
 * assumed. That is the one thing left and it is the expensive one; it belongs to a fresh session.
 * And SwiftUI itself is not the suspect: the Weather and Calendar home-screen widgets are WidgetKit,
 * which is SwiftUI, and they draw their own content on this phone. Whatever stops _JetEngine_SwiftUI
 * is particular to this path.
 *
 * THE THREE SHAPES OF AN INSTRUMENT THAT LIES, all of them met on this branch, all of them making a
 * thing that happened look like a thing that did not:
 *   the wrong channel -- the library was inside AppStore.app and said nothing, because an app
 *     started by SpringBoard has no console on standard error (say() writes to /dev/console now);
 *   the wrong name -- a hook on ams_addCookiesForAccount:clientInfo:bag: never fired for a whole
 *     evening because the cookies go through the four-argument ...:cleanupGlobalCookies: instead;
 *   the wrong moment -- one report at 150 s, while the App Store starts when it is tapped at 95 s
 *     and its report fell past the end of the run (every minute now);
 *   the wrong order -- the hook on JEBrotliDecoder was NOT TAKEN although the class is plainly in
 *     the process (the report's own selector dump lists its methods): the table is tried when dyld
 *     adds an image, and a Swift framework's classes are registered after that callback runs, so
 *     the one moment it was looked for was the one moment it was not there. The table is tried
 *     again every minute now, which costs nothing and cannot miss a late arrival.
 * The fifth shape belongs in this list when it is found.
 */
/*
 * And the decompression, since no signature-failure notification is ever posted (169 to 197
 * notifications in a session of the app, not one of them about a jetpack, a signature, verification
 * or brotli). JEBrotliDecoder is in the App Store's process and not in appstored, and it answers to
 * -decoder and -decompressStreamWithAvailableIn:nextIn:availableOut:nextOut:. The result is an
 * integer status, so it is printed as one: a decode that never runs and a decode that fails are the
 * same silence otherwise.
 */
static long (*old_brotli)(id, SEL, void *, void *, void *, void *);
static long
new_brotli(id self, SEL cmd, void *a, void *b, void *c, void *d)
{
	long result;

	entered[COUNT_brotli]++;
	result = old_brotli(self, cmd, a, b, c, d);
	returned[COUNT_brotli]++;
	if (returned[COUNT_brotli] <= 4 || result != 1) {
		say("hakios: store: BROTLI decompressStream -> %ld (call %lu)\n", result, returned[COUNT_brotli]);
	}
	return result;
}

/*
 * AND THE SHARPER FACT, which was in the task report all along and wanted reading for the thing
 * nobody was looking for: THE APP MAKES EXACTLY FOUR REQUESTS AND NONE OF THEM IS FOR CONTENT.
 * Three sessions, four tasks each, every time the same four: the bag twice, and
 * apps.mzstatic.com/.../ios-appstore.jetpack twice. The jetpack is the store's user interface, not
 * its content; the Today page's items come from a separate feed (amp-api), and no such request is
 * ever made. So "the page is parsed and not drawn" is better said as: nothing ever asks for anything
 * to draw. That moves the question from "why does JetEngine not draw this document" to "what should
 * have asked for the content once the document was loaded, and why does it not".
 * The next thing to measure is accordingly one step earlier: a task that is created and never
 * resumed is a different story from a task that is never created, and they look the same in a report
 * that only counts resumes. Both are counted now -- and the creation hooks fire for nothing, while
 * four resumes are counted, so the four tasks are made somewhere these two do not see. The fact that
 * matters survives either way: four requests, no feed.
 *
 * WHAT IS REACHABLE IN JetEngine FROM A HOOK, since that decides whether the next step is a run or a
 * week of reading. Of 71 042 Objective-C classes in the App Store's process, 40 are named like Jet
 * and 27 like JE, and they are real: JEBrotliDecoder/Encoder, JETreatment, JETreatmentProfile,
 * JETreatmentContext, JETreatmentAction and the JEDeres* actions, JEMediaActivityTracker,
 * JEMetricsDataPredicate, JEUnfairLock, JEWrappedMetricsPipeline, JetEngine itself, JetUI, and --
 * the one worth taking first -- _TtGC9JetEngine7PromiseT__, which is JetEngine's own Promise.
 * The App Store's page is built on promises of its own, and a promise graph is the instrument that
 * found the root in appstored in one slot. The same graph on JetEngine.Promise, with the entry and
 * exit counted apart, is where the next session should start: it says whether anything is waiting
 * for the content that is never asked for, and it costs a run rather than a read.
 */
/*
 * Route the App Store media-account auth (auth.itunes.apple.com) through a host proxy: rewrite the URL
 * here, before the task is made, so the device talks plain HTTP to the host and the host does the real
 * HTTPS to Apple -- we then read every request/response body on the host and pace the calls to avoid
 * HTTP 429. Only auth.itunes is touched. Returns the (possibly rewritten) request.
 */
static void *
rewrite_to_proxy(void *request_)
{
	id request = (id)request_;
	id url = request != NULL ? ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("URL")) : NULL;
	id s = url != NULL ? ((id (*)(id, SEL))objc_msgSend)(url, sel_registerName("absoluteString")) : NULL;
	char u[400] = "";

	if (s != NULL) {
		((char (*)(id, SEL, char *, long, unsigned long))objc_msgSend)(
		    s, sel_registerName("getCString:maxLength:encoding:"), u, (long)sizeof(u), 4);
	}
	if (strstr(u, "https://auth.itunes.apple.com") == u) {
		char nu[460];
		id ns, newurl, mreq;

		snprintf(nu, sizeof(nu), "http://192.168.1.18:8101/_/%s", u + 8 /* strlen("https://") */);
		ns = ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"),
		    sel_registerName("stringWithUTF8String:"), nu);
		newurl = ((id (*)(id, SEL, id))objc_msgSend)((id)objc_getClass("NSURL"),
		    sel_registerName("URLWithString:"), ns);
		mreq = ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("mutableCopy"));
		if (mreq != NULL && newurl != NULL) {
			((void (*)(id, SEL, id))objc_msgSend)(mreq, sel_registerName("setURL:"), newurl);
			((void (*)(id, SEL))objc_msgSend)(mreq, sel_registerName("autorelease"));
			say("hakios: store: proxied -> %s\n", nu);
			return (void *)mreq;
		}
	}
	return request_;
}

static id (*old_dataTaskLocal)(id, SEL, void *, void *);
static id
new_dataTaskLocal(id self, SEL cmd, void *request, void *handler)
{
	id task;
	char buffer[260];

	entered[COUNT_dataTaskCreated]++;
	task = ((id (*)(id, SEL, void *, void *))old_dataTaskLocal)(self, cmd, rewrite_to_proxy(request), handler);
	returned[COUNT_dataTaskCreated]++;
	say("hakios: store: TASK CREATED %s\n", text_of((id)request, buffer, sizeof(buffer)));
	return task;
}

/*
 * One original per row, with no two rows sharing a slot -- the mistake that cost four runs here and
 * is documented beside image_added: the table stores the first original it finds and the other rows
 * then call it with a receiver of another class. Three rows pointed at old_dataTaskLocal and two at
 * old_dataTaskURL, and both daemons aborted from the build that introduced them.
 */
static id (*old_dataTaskAMS)(id, SEL, void *, void *);
static id
new_dataTaskAMS(id self, SEL cmd, void *request, void *handler)
{
	id task;
	char buffer[260];

	entered[COUNT_dataTaskCreated]++;
	task = ((id (*)(id, SEL, void *, void *))old_dataTaskAMS)(self, cmd, rewrite_to_proxy(request), handler);
	returned[COUNT_dataTaskCreated]++;
	say("hakios: store: TASK CREATED (ams session) %s\n", text_of((id)request, buffer, sizeof(buffer)));
	return task;
}

static id (*old_dataTaskURL)(id, SEL, void *, void *);
static id
new_dataTaskURL(id self, SEL cmd, void *url, void *handler)
{
	id task;
	char buffer[260];

	entered[COUNT_dataTaskCreated]++;
	task = ((id (*)(id, SEL, void *, void *))old_dataTaskURL)(self, cmd, url, handler);
	returned[COUNT_dataTaskCreated]++;
	say("hakios: store: TASK CREATED (url) %s\n", text_of((id)url, buffer, sizeof(buffer)));
	return task;
}

/* AMS makes its own, with a signpost: -[AMSURLSession dataTaskWithRequest:signpostID:completionHandler:] */
static id (*old_dataTaskSignpost)(id, SEL, void *, void *, void *);
static id
new_dataTaskSignpost(id self, SEL cmd, void *request, void *signpost, void *handler)
{
	id task;
	char buffer[260];

	entered[COUNT_dataTaskCreated]++;
	task = ((id (*)(id, SEL, void *, void *, void *))old_dataTaskSignpost)(self, cmd, rewrite_to_proxy(request), signpost, handler);
	returned[COUNT_dataTaskCreated]++;
	say("hakios: store: TASK CREATED (ams) %s\n", text_of((id)request, buffer, sizeof(buffer)));
	return task;
}

static id (*old_dataTaskPlain)(id, SEL, void *);
static id
new_dataTaskPlain(id self, SEL cmd, void *request)
{
	id task;
	char buffer[260];

	entered[COUNT_dataTaskCreated]++;
	task = ((id (*)(id, SEL, void *))old_dataTaskPlain)(self, cmd, rewrite_to_proxy(request));
	returned[COUNT_dataTaskCreated]++;
	say("hakios: store: TASK CREATED (no handler) %s\n", text_of((id)request, buffer, sizeof(buffer)));
	return task;
}

/* POST requests carry a body and are made as upload tasks, which the data-task hooks miss -- hook them
 * too and rewrite, so the otp finish/verify POSTs route through the proxy like the GET verify. */
static id (*old_uploadData)(id, SEL, void *, void *, void *);
static id
new_uploadData(id self, SEL cmd, void *request, void *data, void *handler)
{
	id task = ((id (*)(id, SEL, void *, void *, void *))old_uploadData)(self, cmd, rewrite_to_proxy(request), data, handler);
	char buffer[260];

	say("hakios: store: UPLOAD TASK %s\n", text_of((id)request, buffer, sizeof(buffer)));
	return task;
}

static id (*old_uploadData2)(id, SEL, void *, void *);
static id
new_uploadData2(id self, SEL cmd, void *request, void *data)
{
	id task = ((id (*)(id, SEL, void *, void *))old_uploadData2)(self, cmd, rewrite_to_proxy(request), data);
	char buffer[260];

	say("hakios: store: UPLOAD TASK (nh) %s\n", text_of((id)request, buffer, sizeof(buffer)));
	return task;
}

/* and what of JetEngine's own surface is reachable from here at all */
static void
dump_classes(const char *needle)
{
	unsigned count = 0;
	Class *classes = objc_copyClassList(&count);
	unsigned shown = 0;

	if (classes == NULL) {
		return;
	}
	for (unsigned i = 0; i < count && shown < 40; i++) {
		const char *name = class_getName(classes[i]);

		if (name != NULL && strstr(name, needle) != NULL) {
			shown++;
			say("hakios: store: class %s\n", name);
		}
	}
	free(classes);
	say("hakios: store: %u classes in all, %u named like \"%s\"\n", count, shown, needle);
}

/*
 * THE TWO WAITS LEFT IN THE APP, answered the way the device type was. The app's own promise graph
 * shows two threads parked on promises nobody fulfils: one in
 * -[ACAccountStore _updateStorefrontInLocalAccount:] and one in
 * ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies: inside
 * -[AMSURLRequestEncoder requestByEncodingRequest:parameters:]. Both are the same family as the
 * deadlock cut in appstored. Answering them says in one run whether they are on the drawing path,
 * which is far cheaper than reading unnamed Swift in JetEngine without knowing the answer is there.
 * The storefront update answers with the local account at once, as its sibling already does. The
 * cookies answer with nothing at all -- an empty set is a legitimate answer for a device with no
 * account, and the app is asking amsaccountsd for cookies and not being answered. Both return NULL
 * rather than a made-up value: nil is safe to message in Objective-C whatever the real return type
 * is, where a plausible-looking 1 would be a pointer. And both say on the line that they were
 * injected, so that a page drawn with no cookies is never read as a page drawn normally.
 */
static void *(*old_updateStorefront)(id, SEL, void *);
static void *
new_updateStorefront(id self, SEL cmd, void *account)
{
	(void)cmd;
	(void)account;
	(void)self;
	entered[COUNT_updateStorefront]++;
	returned[COUNT_updateStorefront]++;
	/*
	 * nil, not the local account: on iOS 26/27 the caller takes what this answers for a promise and sends it
	 * -promiseAdapter, which an ACAccount does not answer -- the App Store died of that unrecognized selector
	 * at every launch (logs/120122-run.log.live in hakios-sm7325, named by darwin/userland/uncaught.c). nil is
	 * safe to message whatever the real return type is, which is this file's own rule for the answers beside it.
	 */
	/*
	 * ... unless the phone asks for the account instead (HAKIOS_UPDATE_RETURNS_ACCOUNT=1). With the
	 * FairPlay call cut, AMS gets past the device type and reaches a step it never reached before:
	 * "Failed to save account with updated storefront -- No account was provided", twelve times in a
	 * run that had none of it before. nil is what caused that, so the other answer is worth a run: a
	 * finished promise holding the local account, which is what its sibling
	 * (_ams_localiTunesAccountForAccountMediaType:updateStorefront:) already answers with.
	 */
	{
		/*
		 * ... or let AMS do it (HAKIOS_REAL_UPDATE_STOREFRONT=1). The nil here was put in to stop an
		 * unrecognized selector at launch; with FairPlay cut the path has changed under it, so what
		 * the original does now is a question the phone can answer in a minute.
		 */
		const char *real = knob("HAKIOS_REAL_UPDATE_STOREFRONT");

		if (real != NULL && *real != '0' && old_updateStorefront != NULL) {
			static bool said;

			if (!said) {
				said = true;
				say("hakios: store: _updateStorefrontInLocalAccount: left to AMS itself\n");
			}
			return old_updateStorefront(self, cmd, account);
		}
	}
	{
		const char *account_back = knob("HAKIOS_UPDATE_RETURNS_ACCOUNT");

		if (account_back != NULL && *account_back != '0') {
			Class promise_class = objc_getClass("AMSPromise");
			id local = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("ams_localiTunesAccount"));
			id promise = promise_class != Nil ? ((id (*)(id, SEL, id))objc_msgSend)((id)promise_class,
			    sel_registerName("promiseWithResult:"), local) : NULL;
			static bool said;

			if (!said) {
				said = true;
				say("hakios: store: INJECTED _updateStorefrontInLocalAccount: answered a finished "
				    "promise holding %s\n", local != NULL ? "the local account" : "nothing");
			}
			return promise;
		}
	}
	say("hakios: store: INJECTED _updateStorefrontInLocalAccount: answered nil at once, no update\n");
	return NULL;
}

static void *(*old_addCookiesBlocking)(id, SEL, void *, void *, void *, void *);
static void *
new_addCookiesBlocking(id self, SEL cmd, void *account, void *client, void *bag, void *cleanup)
{
	(void)self;
	(void)cmd;
	(void)account;
	(void)client;
	(void)bag;
	(void)cleanup;
	entered[COUNT_addCookiesBlocking]++;
	returned[COUNT_addCookiesBlocking]++;
	say("hakios: store: INJECTED ams_addCookiesForAccount:...:cleanupGlobalCookies: answered with "
	    "NO COOKIES (not measured; the real one waits for ever on amsaccountsd)\n");
	return NULL;
}

/*
 * THE IMAGE THAT OPENS A JETPACK IS JetPack, NOT JetEngine. -[JPReader
 * processStream:signatureVerifier:options:error:] is the entry point, and being an Objective-C
 * method with an error: out-parameter it tells the three remaining stories apart on its own: never
 * called, called and returning an error (and which), or called and succeeding while something later
 * drops the result.
 * And the parser throws C++ exceptions -- JetPack::InvalidSegmentTagException takes the offending
 * segment tag as an int -- which is the best explanation of the silence measured so far: a throw
 * that somebody catches makes no notification, no log line and no decompression. A constructor in
 * another image cannot be replaced from here, but every throw goes through __cxa_throw, which is
 * bound through the stubs and can be interposed; the type's mangled name is the second word of its
 * std::type_info (the Itanium ABI puts __type_name there), so every C++ exception thrown anywhere
 * in the process says what it was.
 */
static id (*old_processStream)(id, SEL, void *, void *, void *, id *);
static id
new_processStream(id self, SEL cmd, void *stream, void *verifier, void *options, id *error)
{
	id result;
	char one[200];

	entered[COUNT_processStream]++;
	say("hakios: store: JETPACK processStream: in\n");
	result = old_processStream(self, cmd, stream, verifier, options, error);
	returned[COUNT_processStream]++;
	{
		/* and who owns the result next: whoever is on the stack when it is handed back */
		char frames[10 * 20 + 1] = "";

		frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
		say("hakios: store: JETPACK processStream: out, %s, error %s, to%s\n",
		    result != NULL ? "a result" : "(nil)",
		    error != NULL && *error != NULL ? text_of(*error, one, sizeof(one)) : "(none)", frames);
	}
	return result;
}

/*
 * THE SEVENTH SHAPE OF AN INSTRUMENT THAT LIES, and the worst of them: ONE THAT CHANGES WHAT IT
 * MEASURES. There was an interpose on __cxa_throw here, to see every C++ exception by name. It
 * answered its question -- std::future_error is AMSPromise being deallocated, see above -- and then,
 * when it was trimmed to print only the first few and count the rest, both daemons began to die:
 *   hakios: appstored: signal 6 ... called from _abort, demangling_terminate_handler,
 *   _objc_terminate, std::__terminate
 * which is an exception reaching std::terminate instead of its handler. appstored went from one
 * start a session to eight, each ending in SIGABRT, and the App Store with it. The runs before that
 * build printed 500 to 600 throws and never aborted once, so it is the hook and not the system: an
 * exception thrown from a C frame of ours, with no unwind information, does not always find its way
 * home. The interpose is gone. What it was for is written down above, and if it is ever wanted
 * again it must be written so that the throw leaves through the original frame, not ours.
 * (It was caught at all only because the fault reporter watches SIGABRT, which it does because of an
 * earlier lesson on this branch. One instrument caught another.)
 */

static unsigned long notifications;

static bool
interesting_name(const char *name)
{
	static const char *const words[] = { "jet", "Jet", "ignature", "erif", "rotli", "JE", "Pack" };

	for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		if (strstr(name, words[i]) != NULL) {
			return true;
		}
	}
	return false;
}

static id (*old_postName)(id, SEL, id, id, id);
static id
new_postName(id self, SEL cmd, id name, id object, id info)
{
	char buffer[160];
	const char *text = text_of(name, buffer, sizeof(buffer));

	notifications++;
	if (interesting_name(text)) {
		say("hakios: store: NOTIFICATION %s\n", text);
	}
	return old_postName(self, cmd, name, object, info);
}

static id (*old_postNotification)(id, SEL, id);
static id
new_postNotification(id self, SEL cmd, id note)
{
	char buffer[160];
	id name = ((id (*)(id, SEL))objc_msgSend)(note, sel_registerName("name"));
	const char *text = text_of(name, buffer, sizeof(buffer));

	notifications++;
	if (interesting_name(text)) {
		say("hakios: store: NOTIFICATION (object) %s\n", text);
	}
	return old_postNotification(self, cmd, note);
}

/*
 * EVERY ERROR THE STORE MAKES, by domain and code, in a table that repeats. After onboarding the
 * App Store draws its own tab bar and then says "Cannot Connect to App Store" with a Retry button
 * (docs/appstore-cannot-connect.png) -- and the task table still shows the same five requests, so
 * whatever it could not connect to was never asked for. The app knows something we do not, and it
 * knows it as an NSError: this remembers the domain and code of every one made in the process, with
 * a count, and prints them in each report. Nothing of the userInfo is printed -- that is where
 * tokens and account details live, and this library is going to run with an Apple ID signed in one
 * day.
 */
#define ERRORS 64
static struct error_note {
	char domain[48];
	long code;
	unsigned count;
} errors[ERRORS];
static unsigned error_count;
static pthread_mutex_t error_lock = PTHREAD_MUTEX_INITIALIZER;

static void
note_error(id domain, long code)
{
	char text[48];

	if (domain == NULL) {
		return;
	}
	text_of(domain, text, sizeof(text));
	pthread_mutex_lock(&error_lock);
	for (unsigned i = 0; i < error_count; i++) {
		if (errors[i].code == code && strncmp(errors[i].domain, text, sizeof(errors[0].domain) - 1) == 0) {
			errors[i].count++;
			pthread_mutex_unlock(&error_lock);
			return;
		}
	}
	if (error_count < ERRORS) {
		strlcpy(errors[error_count].domain, text, sizeof(errors[0].domain));
		errors[error_count].code = code;
		errors[error_count].count = 1;
		error_count++;
	}
	pthread_mutex_unlock(&error_lock);
}

static void
dump_errors(void)
{
	say("hakios: store: %u kinds of error made\n", error_count);
	for (unsigned i = 0; i < error_count; i++) {
		say("hakios: store: error %-40s code %ld, %u time(s)\n", errors[i].domain, errors[i].code,
		    errors[i].count);
	}
}

/* Surface the actual account/store/auth error (domain, code, userInfo) in real time, so the downstream
 * media-account failure after the otp auth succeeds is visible rather than just counted. */
static void
log_live_error(id domain, long code, id info)
{
	char dt[64] = "";

	if (domain == NULL) {
		return;
	}
	text_of(domain, dt, sizeof(dt));
	if (strstr(dt, "AMS") != NULL || strstr(dt, "MZ") != NULL || strstr(dt, "Store") != NULL
	    || strstr(dt, "ccount") != NULL || strstr(dt, "uth") != NULL || strstr(dt, "Tunes") != NULL
	    || strstr(dt, "SSErrorDomain") != NULL) {
		char it[460] = "";

		if (info != NULL) {
			text_of(info, it, sizeof(it));
		}
		say("hakios: store: live-error %s code %ld info %s\n", dt, code, it);
	}
}

static id (*old_errorWith)(id, SEL, void *, long, void *);
static id
new_errorWith(id self, SEL cmd, void *domain, long code, void *info)
{
	note_error((id)domain, code);
	/*
	 * ... and WHO made it. An error says what went wrong and never where: "Failed to save account with
	 * updated storefront -- No account was provided" is said sixteen times in a run and every method of
	 * ours that could be its source is never entered, so the maker is named here by its return address
	 * (unslid, which is what ipsw dyld a2s takes).
	 */
	{
		/* ... and four frames of it: the first is only AMS's error factory, never the caller. */
		unsigned long *frame = (unsigned long *)__builtin_frame_address(0);
		char line[160];
		size_t used = 0;

		for (int i = 0; i < 5 && frame != NULL && ((unsigned long)frame & 7) == 0; i++) {
			int w = snprintf(line + used, sizeof(line) - used, " %lx", frame[1] - slide);

			if (w < 0 || (size_t)w >= sizeof(line) - used) {
				break;
			}
			used += (size_t)w;
			frame = (unsigned long *)frame[0];
		}
		say("hakios: store: an error is made from%s\n", used != 0 ? line : " (nowhere)");
	}
	log_live_error((id)domain, code, (id)info);
	return old_errorWith(self, cmd, domain, code, info);
}

static id (*old_errorInit)(id, SEL, void *, long, void *);
static id
new_errorInit(id self, SEL cmd, void *domain, long code, void *info)
{
	note_error((id)domain, code);
	/*
	 * ... and who made it, the same as the factory above. jetpackassetd makes its NSOSStatusErrorDomain
	 * -50 this way, not through errorWithDomain:, which is why its errors were counted in every report
	 * and never had a maker's name beside them.
	 */
	{
		unsigned long *frame = (unsigned long *)__builtin_frame_address(0);
		char line[160];
		size_t used = 0;

		for (int i = 0; i < 5 && frame != NULL && ((unsigned long)frame & 7) == 0; i++) {
			int w = snprintf(line + used, sizeof(line) - used, " %lx", frame[1] - slide);

			if (w < 0 || (size_t)w >= sizeof(line) - used) {
				break;
			}
			used += (size_t)w;
			frame = (unsigned long *)frame[0];
		}
		say("hakios: store: an error is begun from%s\n", used != 0 ? line : " (nowhere)");
	}
	log_live_error((id)domain, code, (id)info);
	return old_errorInit(self, cmd, domain, code, info);
}

/*
 * THE SIXTH SHAPE OF AN INSTRUMENT THAT LIES: THE WRONG SLICE. The loader's log keeps the first
 * 256 KB and the last 1 MB and throws away the middle when a run overruns it, saying so in one line
 * -- "the log between the first 256 KB and the last 1 MB was dropped". Every run of this library has
 * hit it: 1 466 624 bytes, every time, with the middle gone. Anything printed once, in the middle of
 * a run, was never going to be read, and several measurements here were "missing" for that reason
 * alone and for no reason in the system at all.
 * So nothing that matters is printed once. Keys are remembered and printed again in every report,
 * the way promises and tasks already are, because the reports repeat and a later one lands in the
 * part of the log that is kept. (The other five shapes are listed beside the hooks below.)
 */
#define BAGKEYS 64
static struct bagkey_note {
	char key[72];
	unsigned asked;
	unsigned answered;
} bagkeys[BAGKEYS];
static unsigned bagkey_count;
static pthread_mutex_t bagkey_lock = PTHREAD_MUTEX_INITIALIZER;

static void
note_bagkey(const char *key, bool answered)
{
	pthread_mutex_lock(&bagkey_lock);
	for (unsigned i = 0; i < bagkey_count; i++) {
		if (strncmp(bagkeys[i].key, key, sizeof(bagkeys[0].key) - 1) == 0) {
			bagkeys[i].asked++;
			bagkeys[i].answered += answered ? 1 : 0;
			pthread_mutex_unlock(&bagkey_lock);
			return;
		}
	}
	if (bagkey_count < BAGKEYS) {
		strlcpy(bagkeys[bagkey_count].key, key, sizeof(bagkeys[0].key));
		bagkeys[bagkey_count].asked = 1;
		bagkeys[bagkey_count].answered = answered ? 1 : 0;
		bagkey_count++;
	}
	pthread_mutex_unlock(&bagkey_lock);
}

static void
dump_bagkeys(void)
{
	say("hakios: store: %u bag keys asked for\n", bagkey_count);
	for (unsigned i = 0; i < bagkey_count; i++) {
		say("hakios: store: bag key %-44s asked %u, answered %u%s\n", bagkeys[i].key, bagkeys[i].asked,
		    bagkeys[i].answered, bagkeys[i].answered == 0 ? "   <-- THE BAG HAS NOT GOT IT" : "");
	}
}

/*
 * A GRAPH OF PROMISES, not a list of calls. One promise in seventeen is ever finished here, and
 * naming the next deadlock by hand would take a slot each: what is wanted is the root -- a promise
 * with waiters, no finish, and a producer that is itself waiting on nothing anybody is fulfilling.
 * So every promise this library sees is remembered by address, with where it was first seen (two or
 * three frames of the walk, which "ipsw dyld a2s" names), how many continuations were attached to
 * it, how many threads are blocked in it synchronously and which, and whether it was ever finished.
 * The table is printed at 150 s beside the thread dump, so a blocked thread id in one is the same
 * thread in the other, and a cycle shows up as a cycle instead of as something to recognise.
 */
#define PROMISES 192
struct promise_note {
	void *promise;
	const char *label;
	unsigned waiters;
	unsigned sync_waiters;
	unsigned finished;
	uint64_t tid;                   /* the last thread seen blocked in it */
	char frames[160];
};
static struct promise_note promises[PROMISES];
static unsigned promise_count;
static pthread_mutex_t promise_lock = PTHREAD_MUTEX_INITIALIZER;

/* (the lock is never held across a call into AMS, so this cannot be part of the jam it measures) */
static struct promise_note *
note_for(void *promise, const char *frames)
{
	for (unsigned i = 0; i < promise_count; i++) {
		if (promises[i].promise == promise) {
			return &promises[i];
		}
	}
	if (promise_count >= PROMISES) {
		return NULL;
	}
	promises[promise_count].promise = promise;
	if (frames != NULL) {
		strlcpy(promises[promise_count].frames, frames, sizeof(promises[0].frames));
	}
	return &promises[promise_count++];
}

static void
note_waiter(void *promise, const char *frames, bool synchronous)
{
	struct promise_note *note;
	uint64_t tid = 0;

	pthread_threadid_np(NULL, &tid);
	pthread_mutex_lock(&promise_lock);
	note = note_for(promise, frames);
	if (note != NULL) {
		note->waiters++;
		if (synchronous) {
			note->sync_waiters++;
			note->tid = tid;
		}
	}
	pthread_mutex_unlock(&promise_lock);
}

static void
note_finished(void *promise)
{
	struct promise_note *note;

	pthread_mutex_lock(&promise_lock);
	note = note_for(promise, NULL);
	if (note != NULL) {
		note->finished++;
	}
	pthread_mutex_unlock(&promise_lock);
}

static void
note_label(void *promise, const char *label, const char *frames)
{
	struct promise_note *note;

	pthread_mutex_lock(&promise_lock);
	note = note_for(promise, frames);
	if (note != NULL) {
		note->label = label;
	}
	pthread_mutex_unlock(&promise_lock);
}

static void
dump_promises(void)
{
	say("hakios: store: %u promises seen\n", promise_count);
	for (unsigned i = 0; i < promise_count; i++) {
		struct promise_note *note = &promises[i];

		say("hakios: store: promise %p %-16s waiters %2u sync %u finished %u%s tid %llu seen from%s\n",
		    note->promise, note->label != NULL ? note->label : "", note->waiters, note->sync_waiters,
		    note->finished,
		    note->finished == 0 && note->sync_waiters > 0 ? "  <-- NOBODY FULFILS IT" : "",
		    (unsigned long long)note->tid, note->frames);
	}
}

/*
 * Every thread of the daemon, where it is and how it got there. Seventeen continuations are attached
 * to promises in a session and exactly one promise is ever finished, which is not a fact about the
 * bag: a promise completes when something runs its continuation, and continuations run on dispatch
 * queues, so a queue that is never serviced stops everything behind it. This says what each thread is
 * doing -- waiting in mach_msg for a reply that never comes, parked in __ulock_wait, or not there at
 * all -- and the run state and suspend count tell "blocked" from "never started", which are
 * different problems with different fixes. Nothing is suspended to read it: a snapshot taken by
 * stopping threads could stop one inside the write() this very line needs.
 */
static void
dump_threads(void)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t count = 0;
	mach_port_t me = mach_task_self();

	if (task_threads(me, &threads, &count) != KERN_SUCCESS) {
		say("hakios: store: task_threads() refused\n");
		return;
	}
	/*
	 * ... and where this program itself was loaded, because a thread of appstored sits in
	 * __os_unfair_lock_lock_slow in every report of a run and the frames below it are in appstored's own
	 * binary, not in the shared cache: without its load address those frames cannot be named.
	 */
	/*
	 * ... and where every one of our libraries was loaded. A thread of appstored sits in
	 * __os_unfair_lock_lock_slow under a Swift job in every report of a run, and the frames under it are
	 * neither in the shared cache nor in appstored itself; whether they are in a library of ours decides
	 * whether that deadlock is the daemon's or our own.
	 */
	say("hakios: store: %u threads (this program at %p)\n", count, _dyld_get_image_header(0));
	{
		static bool said;

		if (!said) {
			said = true;
			for (uint32_t i = 0; i < _dyld_image_count(); i++) {
				const char *name = _dyld_get_image_name(i);

				const struct mach_header *header = _dyld_get_image_header(i);
				const char *last = name != NULL ? strrchr(name, '/') : NULL;

				/*
				 * Every image, once: the frames under the blocked thread are in none of the
				 * three obvious ones (the daemon, our two), so the only way to name them is to
				 * have the whole map. The shared cache is left out -- a2s names those.
				 */
				/*
				 * /usr/lib used to be left out as noise, and that is exactly where the maker of
				 * the refusal turned out to live: a frame at 0x10090d4fc fell between two
				 * printed images and could not be named at all. Everything outside the shared
				 * cache is printed now.
				 */
				if (name != NULL && strstr(name, "dyld_shared_cache") == NULL) {
					say("hakios: store: image %p %s\n", header,
					    last != NULL ? last + 1 : name);
				}
			}
		}
	}
	for (unsigned i = 0; i < count; i++) {
		arm_thread_state64_t state;
		mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
		struct thread_basic_info basic;
		mach_msg_type_number_t basic_count = THREAD_BASIC_INFO_COUNT;
		char text[10 * 20 + 1] = "";
		int run_state = -1, suspend = -1;

		if (thread_info(threads[i], THREAD_BASIC_INFO, (thread_info_t)&basic, &basic_count) == KERN_SUCCESS) {
			run_state = basic.run_state;
			suspend = basic.suspend_count;
		}
		if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&state, &state_count) != KERN_SUCCESS) {
			say("hakios: store: thread %u: no state (run %d)\n", i, run_state);
			continue;
		}
		frames_from((uintptr_t)arm_thread_state64_get_fp(state), text, sizeof(text));
		say("hakios: store: thread %u: run %d suspend %d pc %#lx lr %#lx from%s\n", i, run_state, suspend,
		    (uintptr_t)arm_thread_state64_get_pc(state) - slide,
		    (uintptr_t)arm_thread_state64_get_lr(state) - slide, text);
	}
	for (unsigned i = 0; i < count; i++) {
		mach_port_deallocate(me, threads[i]);
	}
	vm_deallocate(me, (vm_address_t)threads, count * sizeof(threads[0]));
}

static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, lr = 0, fp = 0;
	char frames[10 * 20 + 1] = "";

	if (context != NULL && context->uc_mcontext != NULL) {
		/* arm64e: the thread state's pc/lr/fp are PAC-signed; the accessors strip the signature */
		pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(context->uc_mcontext->__ss);
		lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(context->uc_mcontext->__ss);
		fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(context->uc_mcontext->__ss);
	}
	frames_from(fp, frames, sizeof(frames));
	say("hakios: store: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void
watch_for_faults(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = say_the_fault;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	/* an ObjC exception ends in abort(), and a bad replacement of ours is likelier to do that than
	   to touch bad memory: without these a daemon that dies of us leaves nothing at all */
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
}

/* ---- the storefront, answered ----------------------------------------------------------------- */

static id (*old_ams_storefront)(id, SEL);
static id (*old_ams_storefront_media)(id, SEL, id);
static id (*old_cached_storefront)(id, SEL);
static id (*old_bag_storefront)(id, SEL, id);
static id (*old_bag_storefront_active)(id, SEL, id);
static id (*old_copy_storefront)(id, SEL, id);
static id (*old_bag_account)(id, SEL, id, bool);
static id (*old_perform_fetch)(id, SEL, long, id, id);
static id (*old_query_items)(id, SEL, id, id, id, id, id, id);
static id (*old_perform_request)(id, SEL, id, id);
static id (*old_perform_request_block)(id, SEL, id, id);
static void (*old_resume_base)(id, SEL);
static void (*old_resume_local)(id, SEL);
static id (*old_create_request)(id, SEL, id);
static id (*old_session_promise)(id, SEL);
static id (*old_data_task_promise)(id, SEL, id);
static id (*old_data_task)(id, SEL, id);

static id
new_ams_storefront(id self, SEL _cmd)
{
	entered[COUNT_ams_storefront]++;
	id was = old_ams_storefront != NULL ? old_ams_storefront(self, _cmd) : NULL;
	static bool said;

	if (!said) {
		char buffer[128];

		said = true;
		say("hakios: store: ams_storefront was %s, now " STOREFRONT "\n", text_of(was, buffer, sizeof(buffer)));
	}
	returned[COUNT_ams_storefront]++;
	return was != NULL ? was : (id)storefront;
}

/*
 * The same answer for the accessor that takes a media type. -[ACAccount ams_storefront] is the door most of
 * AMS goes through, but not the one that decides whether an account counts as having a storefront at all:
 * -[AMSBagActiveAccountProvider _handleFetchedLocalAccount:fetchError:mustContainStorefront:mediaType:error:]
 * (0x18c836398 on 24B119) asks the account -ams_storefrontForMediaType: and, with nothing back, makes
 * AMSErrorDomain 7 "Local account found but has missing storefront" -- which is in every run's log while
 * ams_storefront is answered eighteen times. So the two are hooked together; with only one of them the
 * account the bag provider hands on has no storefront and the chain stops there.
 */
static id
new_ams_storefront_media(id self, SEL _cmd, id media_type)
{
	entered[COUNT_storefrontMedia]++;
	id was = old_ams_storefront_media != NULL ? old_ams_storefront_media(self, _cmd, media_type) : NULL;
	static bool said;

	if (!said) {
		char one[64], two[128];

		said = true;
		say("hakios: store: ams_storefrontForMediaType:%s was %s, now " STOREFRONT "\n",
		    text_of(media_type, one, sizeof(one)), text_of(was, two, sizeof(two)));
	}
	returned[COUNT_storefrontMedia]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_cached_storefront(id self, SEL _cmd)
{
	entered[COUNT_cachedStorefront]++;
	id was = old_cached_storefront != NULL ? old_cached_storefront(self, _cmd) : NULL;

	say("hakios: store: cachedStorefront was %s\n", was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_cachedStorefront]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_bag_storefront(id self, SEL _cmd, id media_type)
{
	entered[COUNT_bagStorefront_fixed]++;
	id was = old_bag_storefront != NULL ? old_bag_storefront(self, _cmd, media_type) : NULL;
	char buffer[128];

	say("hakios: store: bagStorefrontForAccountMediaType:%s was %s\n", text_of(media_type, buffer, sizeof(buffer)),
	    was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_bagStorefront_fixed]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_bag_storefront_active(id self, SEL _cmd, id media_type)
{
	entered[COUNT_bagStorefront_active]++;
	id was = old_bag_storefront_active != NULL ? old_bag_storefront_active(self, _cmd, media_type) : NULL;
	char buffer[128];

	say("hakios: store: active bagStorefrontForAccountMediaType:%s was %s\n",
	    text_of(media_type, buffer, sizeof(buffer)), was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_bagStorefront_active]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_copy_storefront(id self, SEL _cmd, id account)
{
	entered[COUNT_copyStorefront]++;
	id was = old_copy_storefront != NULL ? old_copy_storefront(self, _cmd, account) : NULL;

	say("hakios: store: ams_copyStorefrontFromAccount: was %s\n",
	    was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_copyStorefront]++;
	return was != NULL ? was : (id)storefront;
}

/* not replaced, watched: this is the fallback line, and which way it goes is the question */
static id
new_bag_account(id self, SEL _cmd, id media_type, bool must_contain_storefront)
{
	entered[COUNT_bagAccount]++;
	id account = old_bag_account(self, _cmd, media_type, must_contain_storefront);
	returned[COUNT_bagAccount]++;
	char one[128], two[192];

	say("hakios: store: _bagAccountForAccountMediaType:%s mustContainStorefront:%d -> %s\n",
	    text_of(media_type, one, sizeof(one)), must_contain_storefront ? 1 : 0, text_of(account, two, sizeof(two)));
	return account;
}

/* ---- is a request ever built? ------------------------------------------------------------------ */

static id
new_perform_fetch(id self, SEL _cmd, long attempt, id account, id front)
{
	entered[COUNT_performFetch]++;
	char one[160], two[64];

	id promise;

	say("hakios: store: BAG FETCH attempt %ld, storefront %s, account %s\n", attempt,
	    text_of(front, two, sizeof(two)), text_of(account, one, sizeof(one)));
	for (size_t i = 0; i < replacement_count(); i++) {
		if (!replacements[i].done) {
			say("hakios: store: NOT TAKEN %c[%s %s]\n", replacements[i].class_method ? '+' : '-',
			    replacements[i].class_name, replacements[i].selector);
		}
	}
	promise = old_perform_fetch(self, _cmd, attempt, account, front);
	returned[COUNT_performFetch]++;
	note_label(promise, "THE BAG FETCH", NULL);
	say("hakios: store: BAG FETCH returned %s\n", promise != NULL ? "a promise" : "(nil)");
	return promise;
}

static id
new_query_items(id self, SEL _cmd, id client, id profile, id version, id cookies, id account, id front)
{
	entered[COUNT_queryItems]++;
	id items = old_query_items(self, _cmd, client, profile, version, cookies, account, front);
	char one[64], two[160];

	say("hakios: store: BAG QUERY profile %s, storefront %s -> %s\n", text_of(profile, one, sizeof(one)),
	    text_of(front, two, sizeof(two)), items != NULL ? "items" : "(nil)");
	return items;
}

/* the session layer: a request that is built but never sent stops between these and the wire */
static id
new_perform_request(id self, SEL _cmd, id request, id completion)
{
	entered[COUNT_performRequest]++;
	char buffer[320];

	say("hakios: store: AMSURLSession performRequest:completion: %s\n", text_of(request, buffer, sizeof(buffer)));
	return old_perform_request(self, _cmd, (id)rewrite_to_proxy((void *)request), completion);
}

static id
new_perform_request_block(id self, SEL _cmd, id request, id completion)
{
	entered[COUNT_performRequestBlock]++;
	char buffer[320];

	say("hakios: store: AMSURLSession performRequest:completionBlock: %s\n", text_of(request, buffer, sizeof(buffer)));
	return old_perform_request_block(self, _cmd, (id)rewrite_to_proxy((void *)request), completion);
}

/*
 * And the narrowest gate of all: every transfer, foreground or background, in process or handed to
 * nsurlsessiond, becomes a task that somebody resumes. If the bag request is built and no task is
 * ever resumed, it never became a transfer at all; if one is, the loss is below CFNetwork.
 */
static void
say_resume(id task, const char *where)
{
	char buffer[320];
	id request = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("originalRequest"));

	say("hakios: store: TASK RESUME (%s) %s\n", where, text_of(request, buffer, sizeof(buffer)));
}

/*
 * Signing the storefront token request at the last possible moment. The task that carries it is a
 * __NSCFLocalSessionTask and it is certain to pass through resume -- the builder that makes the
 * request is a StoreServices internal that none of the creation hooks above ever see, so this is the
 * one place to reach it. A task's request is immutable, but the task has not yet handed it to
 * CFNetwork at the instant resume is entered, and the concrete task answers the private
 * -_setCurrentRequest:, so this swaps in a mutable copy carrying the device's anisette. The anisette
 * itself is put on by Apple's own SSVAnisetteAddHeadersToURLRequest, which draws the one-time
 * password from adid over the same ADI path a working Apple-ID sign-in uses (the one Apple's servers
 * already accepted from this phone). Guarded to the apiToken URL, to a request not already signed,
 * and to a task that answers the setter.
 */
typedef void (*ssv_anisette_fn)(id, id *);
static ssv_anisette_fn ssv_anisette;

static void
sign_token_request(id task)
{
	id request, url, mut, existing, error = NULL;
	char urltext[260];
	SEL setter = sel_registerName("_setCurrentRequest:");

	if (ssv_anisette == NULL) {
		return;
	}
	request = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("currentRequest"));
	if (request == NULL) {
		return;
	}
	url = ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("URL"));
	{
		static int shown;	/* diagnostic: what URL every task actually has at resume */

		if (shown < 20) {
			shown++;
			say("hakios: store: RESUME URL [%s]\n", url != NULL ? text_of(url, urltext, sizeof(urltext)) : "(none)");
		}
	}
	if (url == NULL || strstr(text_of(url, urltext, sizeof(urltext)), "apiToken") == NULL) {
		return;
	}
	existing = ((id (*)(id, SEL, CFStringRef))objc_msgSend)(request,
	    sel_registerName("valueForHTTPHeaderField:"), CFSTR("X-Apple-I-MD"));
	if (existing != NULL) {
		return;		/* already signed on an earlier resume of the same task */
	}
	if (!((bool (*)(id, SEL, SEL))objc_msgSend)(task, sel_registerName("respondsToSelector:"), setter)) {
		say("hakios: store: token resume: task will not answer _setCurrentRequest:, cannot sign here\n");
		return;
	}
	mut = ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("mutableCopy"));
	if (mut == NULL) {
		return;
	}
	ssv_anisette(mut, &error);
	((void (*)(id, SEL, id))objc_msgSend)(task, setter, mut);		/* _setCurrentRequest: retains it */
	existing = ((id (*)(id, SEL, CFStringRef))objc_msgSend)(mut,
	    sel_registerName("valueForHTTPHeaderField:"), CFSTR("X-Apple-I-MD"));
	say("hakios: store: token resume: anisette %s on the apiToken request, ssv error %p\n",
	    existing != NULL ? "ATTACHED" : "STILL MISSING", (void *)error);
}

static void
new_resume_base(id self, SEL _cmd)
{
	entered[COUNT_resume]++;
	say_resume(self, "NSURLSessionTask");
	remember_task(self);
	sign_token_request(self);
	old_resume_base(self, _cmd);
	returned[COUNT_resume]++;
}

static void
new_resume_local(id self, SEL _cmd)
{
	entered[COUNT_resumeLocal]++;
	say_resume(self, "__NSCFLocalSessionTask");
	sign_token_request(self);
	old_resume_local(self, _cmd);
}

/*
 * What a class really answers to. The bag request is built and no task is ever resumed, so the stop
 * is between the two, in AMS's own session layer -- whose selectors are not the ones guessed from
 * the cache's strings (AMSURLSession has no performRequest:completion:). Rather than guess again,
 * ask the runtime, late enough that the console is no longer dropping lines.
 */
static void
dump_selectors(const char *class_name, const char *needle)
{
	Class cls = objc_getClass(class_name);
	unsigned count = 0;
	Method *methods = cls != NULL ? class_copyMethodList(cls, &count) : NULL;

	if (methods == NULL) {
		say("hakios: store: %s: no such class\n", class_name);
		return;
	}
	for (unsigned i = 0; i < count; i++) {
		const char *name = sel_getName(method_getName(methods[i]));

		if (needle == NULL || strstr(name, needle) != NULL) {
			say("hakios: store: %s has -%s\n", class_name, name);
		}
	}
	free(methods);
}

static int unlocked_since_boot_now(void);       /* defined with the keybag hook, below */

static void
one_report(unsigned seconds)
{
	bool first = seconds <= 15;

	say("hakios: store(%s): report at %u s\n", who, seconds);
	image_added(NULL, 0);           /* and try again for anything whose class has turned up since */
	/*
	 * These two on their own, before the long report: last time the brotli counter was one line of
	 * sixty and it went missing, and a missing line is what this branch has been wrong about five
	 * times. A number that matters does not travel in a crowd.
	 */
	say("hakios: store: KEY brotli entered %lu returned %lu, processStream entered %lu returned %lu\n",
	    entered[COUNT_brotli], returned[COUNT_brotli], entered[COUNT_processStream], returned[COUNT_processStream]);
	/* ... and the one answer jetpackassetd waits on before it does anything at all */
	say("hakios: store(%s): KEY the device has been unlocked since boot: %d\n", who,
	    unlocked_since_boot_now());
	/*
	 * ... and the three ways a process can be asked what bundle it is. The App Store's own log says
	 * "Client bundle id com.apple.AppStore (executable: /Applications/AppStore.app/AppStore) is nil, not
	 * allow listed or not entitled to use signing service" and then "Unable to create pool manager, not
	 * a valid client": the signing service refuses because it cannot identify its client, and the thing
	 * that cannot identify it is LaunchServices -- +[LSBundleRecord bundleRecordForCurrentProcess] is
	 * the most-made error in the whole run (409 of them, from LSMakeNSErrorImpl), under containers that
	 * answer NOT_CODESIGNED. Which of the three still works decides whether this is repairable from
	 * inside the process or not.
	 */
	{
		Class record = objc_getClass("LSBundleRecord");
		Class proxy = objc_getClass("LSBundleProxy");
		id main_bundle = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSBundle"),
		    sel_registerName("mainBundle"));
		id from_bundle = main_bundle != NULL ? ((id (*)(id, SEL))objc_msgSend)(main_bundle,
		    sel_registerName("bundleIdentifier")) : NULL;
		SEL rec_sel = sel_registerName("bundleRecordForCurrentProcess");
		SEL prx_sel = sel_registerName("bundleProxyForCurrentProcess");
		/* ... and only if the class really answers it: a selector that is not there is a crash, and a
		   crash in the report would take the whole process with it */
		id from_record = (record != Nil && class_getClassMethod(record, rec_sel) != NULL)
		    ? ((id (*)(Class, SEL))objc_msgSend)(record, rec_sel) : NULL;
		id from_proxy = (proxy != Nil && class_getClassMethod(proxy, prx_sel) != NULL)
		    ? ((id (*)(Class, SEL))objc_msgSend)(proxy, prx_sel) : NULL;
		char a[120] = "(nil)", b[160] = "(nil)", c[160] = "(nil)";

		say("hakios: store(%s): KEY what bundle am I: NSBundle %s, LSBundleRecord %s, LSBundleProxy %s\n",
		    who, from_bundle != NULL ? text_of(from_bundle, a, sizeof(a)) : a,
		    from_record != NULL ? text_of(from_record, b, sizeof(b)) : b,
		    from_proxy != NULL ? text_of(from_proxy, c, sizeof(c)) : c);
	}
	{
		/*
		 * ... and the answer underneath all of it. The containers say NOT_CODESIGNED (error 98) about
		 * these processes, and that is what LaunchServices and the signing service both lean on. What
		 * Security itself says about this process is the measurement that tells a repairable identity
		 * from a missing signature.
		 */
		void *(*task_self)(void) = (void *(*)(void))dlsym(RTLD_DEFAULT, "SecTaskCreateFromSelf");
		void *(*copy_id)(void *) = (void *(*)(void *))dlsym(RTLD_DEFAULT,
		    "SecTaskCopySigningIdentifier");
		void *task = NULL, *ident = NULL;
		char d[140] = "(nil)";

		if (task_self != NULL && copy_id != NULL) {
			/* SecTaskCreateFromSelf takes an allocator; NULL is the default one */
			void *(*make)(void *) = (void *(*)(void *))task_self;

			task = make(NULL);
			if (task != NULL) {
				void *(*copy)(void *, void *) = (void *(*)(void *, void *))copy_id;

				ident = copy(task, NULL);
			}
		}
		say("hakios: store(%s): KEY Security says this process is signed as %s\n", who,
		    ident != NULL ? text_of((id)ident, d, sizeof(d)) : (task_self == NULL ? "(no SecTask here)"
		    : task == NULL ? "(no task object)" : "NOTHING"));
	}

	for (size_t i = 0; i < replacement_count(); i++) {
		if (!replacements[i].done) {
			say("hakios: store: NOT TAKEN %c[%s %s]\n", replacements[i].class_method ? '+' : '-',
			    replacements[i].class_name, replacements[i].selector);
		}
	}
	for (int i = 0; i < COUNT_last; i++) {
		/*
		 * A hook that never fires says so loudly, once. Silence from one is not evidence about the
		 * code: ams_addCookiesForAccount:clientInfo:bag: was hooked and never entered all night
		 * because the cookies go through the FOUR-argument
		 * ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies: instead. Hooked and never
		 * entered can mean the hook is wrong, not the code.
		 */
		say("hakios: store: entered %4lu returned %4lu %s%s\n", entered[i], returned[i], counted[i],
		    entered[i] > returned[i] ? "   <-- STILL IN IT" :
		    entered[i] == 0 ? "   <-- NEVER ENTERED: suspect the hook, not the code" : "");
	}
	dump_promises();
	dump_tasks();
	dump_bagkeys();
	dump_errors();
	dump_threads();
	dump_tracked_promises();
	dump_tracked_conts();
	dump_prereq_types();
	force_resolve_prereqs();
	/* the long dumps once; the numbers in every report, because only a late one survives the log */
	if (first) {
		dump_selectors("AMSURLSession", "equest");
		dump_selectors("JEBrotliDecoder", NULL);
		dump_selectors("_TtGC9JetEngine7PromiseT__", NULL);
		dump_selectors("JetEngine", NULL);
		dump_classes("Jet");
		dump_classes("JE");
	}
	say("hakios: store: %lu notifications posted\n", notifications);
}

/*
 * Every minute rather than once. appstored starts with the system and the App Store starts when
 * somebody taps it, a minute and a half later, so a single report at 150 s falls past the end of
 * the run for one of them -- and the app's side of the story went missing in exactly that way.
 */
/*
 * What this process calls itself. AdPlatforms says "Client bundle id com.apple.AppStore (executable: ...)
 * is nil, not allow listed or not entitled to use signing", which reads as though the bundle identifier
 * were missing, and a client that cannot name itself would ask for nothing. Said plainly here, so the
 * wording of that line is not taken for a measurement.
 *
 * NOT FROM THE CONSTRUCTOR. A library dyld inserts is initialized before the frameworks only the main
 * program asks for, and this one links CoreFoundation, not Foundation: in appstored -- which gets this
 * library and the Mescal one and nothing else -- NSBundle here ran before Foundation had initialized, and
 * +[NSString stringWithFormat:] inside it met an NSString that was still the abstract class, so Foundation
 * called __NSRequestConcreteObject to complain, which formats its complaint with stringWithFormat, which
 * met the same abstract class... appstored overflowed its stack in that loop and launchd restarted it into
 * the same crash, 43 times in one run (logs/172213-run.log), so nothing served the App Store's requests.
 * The App Store itself hid this: every application also gets libhakios-statusbar.dylib, which does link
 * Foundation and therefore initializes it first. Said from the reporting thread instead, long after
 * everything is up. (The library now links Foundation as well, which fixes the order; this stays, because
 * a constructor is the wrong place to ask a framework anything.)
 */
static void
tell_who_i_am(void)
{
	id bundle = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSBundle"), sel_registerName("mainBundle"));
	id ident = bundle != NULL ? ((id (*)(id, SEL))objc_msgSend)(bundle, sel_registerName("bundleIdentifier")) : NULL;
	id path = bundle != NULL ? ((id (*)(id, SEL))objc_msgSend)(bundle, sel_registerName("bundlePath")) : NULL;
	char one[120], two[200];

	say("hakios: store: this process calls itself %s at %s\n",
	    ident != NULL ? text_of(ident, one, sizeof(one)) : "(nil)",
	    path != NULL ? text_of(path, two, sizeof(two)) : "(nil)");
}

static void *
late_report(__unused void *ignored)
{
	sleep(5);
	tell_who_i_am();
	if (who != NULL && strcmp(who, "accountsd") == 0) {
		make_account_types();   /* after the daemon has its own database open */
	}
	/*
	 * Every fifteen seconds, to the end of the run. The loader keeps the first 256 KB and the last
	 * 1 MB of a run's log and drops everything between, and with iOS and REST=1 the middle is most
	 * of it: a report once a minute still fell inside the hole, twice in a row, which is why these
	 * measurements kept looking missing when nothing was wrong with them. Often enough that one
	 * lands in the last megabyte is the only thing that works, so the long dumps happen once and the
	 * numbers repeat.
	 */
	for (unsigned n = 1; n <= 40; n++) {
		sleep(15);
		one_report(n * 15);
	}
	return NULL;
}

/*
 * The four steps between a bag request and a transfer, as the runtime says AMS really spells them:
 * AMSBagNetworkTask builds the request (_createRequestWithQueryItems:), asks for its session
 * (URLSessionPromise -- made lazily, and AMS has a line about being deallocated "while lazily
 * instantiating its AMSURLSession"), and the session makes a task (dataTaskPromiseWithRequest: or
 * dataTaskWithRequest:) which somebody resumes. resume is never reached, so the stop is one of
 * these; each says what it was given and what it gave back.
 */
static id
new_create_request(id self, SEL _cmd, id items)
{
	id request;
	char buffer[320];

	entered[COUNT_createRequest]++;
	request = old_create_request(self, _cmd, items);
	returned[COUNT_createRequest]++;
	say("hakios: store: STEP _createRequestWithQueryItems: -> %s\n", text_of(request, buffer, sizeof(buffer)));
	note_label(request, "THE BAG REQUEST", NULL);
	return request;
}

static id
new_session_promise(id self, SEL _cmd)
{
	id promise;
	char buffer[200];

	entered[COUNT_sessionPromise]++;
	promise = old_session_promise(self, _cmd);
	returned[COUNT_sessionPromise]++;
	say("hakios: store: STEP URLSessionPromise -> %s\n", text_of(promise, buffer, sizeof(buffer)));
	return promise;
}

/*
 * The storefront API token request (sf-api-token-service/apiToken) is the one that returns 401, and
 * the header dump says why: it carries a storefront but NO authorization at all, because the AppSSO
 * authorization its own code asks for fails -1000 and attaches nothing. Everything below AMS is
 * cleared and the storefront is right, so the only thing missing on the wire is the device's
 * anisette -- which the store DOES attach to its bag requests through
 * ams_addAnisetteHeadersForAccount:type:bag:, and which this device can make (Apple's own servers
 * accepted it for the Apple ID password check). new_addAnisette below remembers the account, media
 * type and bag the store passed when it anisette-signed a bag request; this reuses exactly those on
 * a mutable copy of the token request, so the request carries the credential instead of nothing.
 * That turns "we sent no credential -> 401" into a real test: a 200 with a token means the 401 was
 * our silence; a 401 still means the server wants more than anisette (the attestation wall).
 */
static void *(*old_addAnisette)(id, SEL, void *, void *, void *);
static SEL anisette_cmd;
static void *anisette_account, *anisette_type, *anisette_bag;

/*
 * The Mescal signature on iOS 26/27 is put on by +[AMSURLRequestDecoration addAbsintheHeadersToRequest:
 * buyParams:bag:shouldUseRemoteSigningIfAvailable:], which needs the bag: the bag's "absinthe-requests"
 * says which requests are signed at all, and it is what finally asks -[AMSMescalSession signData:bag:error:]
 * (our sapd). The storefront-token request is built with no bag ("Skipping GUID parameter addition as bag
 * was not provided"), so nothing is signed and the token is refused. This says whether the absinthe step is
 * reached and what bag it is handed; a bag seen anywhere else is kept so it can be handed over here.
 */
static id seen_bag;

static void
keep_bag(id bag, const char *where)
{
	static int told;

	if (bag == NULL || seen_bag != NULL) {
		return;
	}
	seen_bag = (id)CFRetain((CFTypeRef)bag);
	if (!told) {
		told = 1;
		say("hakios: store: a bag is kept from %s, to hand to the absinthe step\n", where);
	}
}

/*
 * Who decides the storefront-token request is signed at all. The absinthe step above is reached only for
 * the bag's own request; the token request never goes through it, so nothing asks for a Mescal signature
 * and the token is refused. In StoreServices the decision is -[SSURLConnectionRequest shouldMescalSign]
 * (its policy comes from -[SSVURLBagInterpreter mescalSignaturePolicyForRequestURL:]). This says what it
 * answers and for which URL, and answers YES for the token request, so the signing path is entered and
 * reaches -[AMSMescalSession signData:bag:error:] -- ours, which asks sapd.
 *
 * MEASURED, AND IT IS NOT THIS: on iOS 27 the hook is taken and NEVER entered (logs/124717-run.log.live),
 * so the App Store's token request is not a StoreServices request at all. It is kept to say so: the
 * request is AMS's, and what decides AMS does not sign it is still to be found.
 */
static BOOL (*old_should_mescal)(id, SEL);

static BOOL
new_should_mescal(id self, SEL cmd)
{
	BOOL was = old_should_mescal != NULL ? old_should_mescal(self, cmd) : NO;
	char text[300] = "(no URL)";
	id url = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("URL"));
	bool is_token;

	if (url != NULL) {
		text_of(url, text, sizeof(text));
	} else {
		text_of(self, text, sizeof(text));
	}
	is_token = strstr(text, "apiToken") != NULL;
	say("hakios: store: shouldMescalSign %d for %.150s%s\n", was ? 1 : 0, text,
	    is_token && !was ? "  -> answered YES (the token request)" : "");
	return (is_token && !was) ? YES : was;
}

/*
 * The storefront header IS put on the token request (it is among its 8), so that decoration runs while the
 * request is still mutable and with a bag in hand -- which is what the absinthe (Mescal) step needs and
 * never gets. This hooks the storefront step, and for the token request asks the absinthe step with the
 * same bag (or the kept one), so the signature is made where AMS never asks for it.
 */
/*
 * What appstored itself waits on. It logs "[Bag] Fetching current account", gets the account (null) and
 * never speaks again, and none of the hooks above are entered in it -- because it does not ask for the
 * local account at all: its binary names -[ACAccountStore ams_activeiTunesAccountForMediaType:] (and
 * ams_activeiTunesAccount). Those are answered here with the local account as it is, so the daemon stops
 * waiting and its XPC answers the App Store again.
 */
static id (*old_active_account)(id, SEL);
static id (*old_active_account_type)(id, SEL, void *);

/*
 * The wait that holds the App Store. A thread of its own sits for ever in
 *   +[AMSSyncRestrictedRegionTask performSync] -> durationOfSyncRestrictedRegionValueFromBag:
 *   -> -[AMSBagValue valueWithError:] -> -[AMSPromise resultWithError:] -> resultBeforeDate:error:
 *   -> -[NSCondition waitUntilDate:]
 * -- a synchronous read of a bag value whose promise is never finished, which is the same family of
 * wait this file was written to cut on 16.5. The task syncs whether the storefront is in a restricted
 * region; nothing about the catalog depends on it here. Both values it reads are answered at once,
 * without touching the bag, so the thread never blocks: no restricted region, and no interval to sync on.
 */
static IMP old_restricted_region;

/*
 * A SWITCH THE PHONE CAN BE GIVEN WITHOUT BUILDING AN IMAGE. Every switch here used to come from the boot
 * line (exec_env=NAME:VAR=1), which means a new image and a new boot for each one: eight minutes a question,
 * nearly all of it waiting. /var/hakios-store.conf is read instead, once per process, as lines NAME=VALUE
 * (# starts a comment); a switch that is not in the file falls back to the environment, so the boot line
 * still works. /var is kept across runs (var_store=1) and is writable over SSH, so a question now costs
 * writing a line, killing the application and opening it again -- seconds, inside one boot.
 */
static const char *
knob(const char *name)
{
	static char text[2048];
	static size_t length;
	static bool read_already;
	size_t want = strlen(name);

	if (!read_already) {
		int fd = open("/var/hakios-store.conf", O_RDONLY);
		ssize_t got = fd >= 0 ? read(fd, text, sizeof(text) - 1) : -1;

		read_already = true;
		if (fd >= 0) {
			close(fd);
		}
		length = got > 0 ? (size_t)got : 0;
		text[length] = 0;
		for (size_t i = 0; i < length; i++) {
			if (text[i] == '\n' || text[i] == '\r' || text[i] == '#') {
				text[i] = 0;
			}
		}
	}
	for (size_t i = 0; i < length;) {
		const char *line = text + i;
		size_t n = strlen(line);

		if (n > want && strncmp(line, name, want) == 0 && line[want] == '=') {
			return line + want + 1;
		}
		i += n + 1;
	}
	return getenv(name);
}

static long long
no_restricted_region(id self, SEL cmd, void *bag)
{
	static int told;
	const char *keep = knob("HAKIOS_KEEP_REGION_WAIT");

	if (keep != NULL && *keep != '0') {
		return ((long long (*)(id, SEL, void *))old_restricted_region)(self, cmd, bag);
	}

	if (!told) {
		told = 1;
		say("hakios: store: the restricted-region sync is answered at once: no region, no interval\n");
	}
	return 0;
}

/*
 * Every Mach service this process opens, including the ones opened by the C interface. The NSXPCConnection
 * hook below names only the Objective-C ones, and AppleMediaServices' Swift code opens its own with
 * xpc_connection_create_mach_service -- which is the kind of call appstored is stuck in. An inline hook
 * (the prefix's MSHookFunction, as inject_kc.c uses) names those too, so the service that never answers
 * can be named instead of guessed.
 */
typedef void (*MSHookFunction_t)(void *symbol, void *replace, void **result);
typedef void *(*xpc_mach_service_t)(const char *, void *, unsigned long);
static xpc_mach_service_t old_xpc_mach_service;

/* which connection is which, so a message that never comes back can be named by its service */
#define SERVICES 32
static struct { void *connection; char name[72]; } service_of[SERVICES];
static unsigned services_known;

static const char *
service_name(void *connection)
{
	for (unsigned i = 0; i < services_known && i < SERVICES; i++) {
		if (service_of[i].connection == connection) {
			return service_of[i].name;
		}
	}
	return "(a connection we did not see opened)";
}

static void *
new_xpc_mach_service(const char *name, void *queue, unsigned long flags)
{
	void *connection = old_xpc_mach_service(name, queue, flags);

	say("hakios: store: mach service opened: %s\n", name != NULL ? name : "(no name)");
	if (connection != NULL && name != NULL && services_known < SERVICES) {
		service_of[services_known].connection = connection;
		snprintf(service_of[services_known].name, sizeof(service_of[0].name), "%s", name);
		services_known++;
	}
	return connection;
}

/*
 * A synchronous message and its answer were named here too ("asks SERVICE" before the send, "answered
 * by SERVICE" after), and that measurement is done: in appstored every sync message is answered
 * (mobileactivationd, accountsd, cfprefsd, runningboard) and in the App Store all ninety-six are, so
 * neither is deadlocked on XPC. The hook is gone because it cannot be left on: saying anything inside it
 * sends another synchronous message wherever a process's os_log goes to logd over XPC rather than to
 * standard error, and appstored died of that recursion -- ten stack overflows in one run, where the run
 * before, whose only difference was that its os_log went to standard error, had none. A re-entrancy guard
 * cut it to one and silenced the measurement, which is the shape of a diagnostic that breaks what it
 * measures. Naming the services a process opens (above) is safe and stays.
 */

/*
 * THE XPC SESSIONS JetEngine OPENS. The App Store's page is drawn by JetEngine (JetUI.JULoadingViewController
 * is the "Loading..." the screen stays on), and JetEngine talks to its daemon -- jetpackassetd, which keeps
 * the JetPack bundles the pages are made of -- over an XPC *session*, the Swift one, not NSXPCConnection:
 * JetEngine/DaemonSession.swift. Its line "[Daemon] XPC session (sendSync) cancelled: { Error
 * domain=XPC.XPCRichError, code=1 }" is in every run, a second after the app opens, and after it the app
 * asks the network for nothing at all. A session is a different pair of functions from a connection, which
 * is why naming connections never showed it, so the three that matter are named here with the rich error
 * each one gives back. Safe to leave on: say() writes to /dev/console with write(2) and sends no message.
 */
typedef char *(*rich_desc_fn)(void *);
static rich_desc_fn rich_desc;
static rich_desc_fn xpc_desc;           /* xpc_copy_description: what was asked and what came back */
static void *(*old_session_create)(const char *, void *, unsigned long long, void **);
static bool (*old_session_activate)(void *, void **);
static void *(*old_session_send_sync)(void *, void *, void **);

/*
 * WHICH session a message goes to. The App Store makes one session to jetpackassetd by name and then
 * activates a second one that was never created by name here, and "initialize" -- the message JetEngine
 * sends its daemon and after which everything stops -- is sent one way on one of them. Without the pair
 * (session, name) the two are indistinguishable, and "the daemon was told" and "the message went to the
 * defaults service" read the same in the log. So every session made by name is remembered by its pointer.
 */
#define SESSIONS 32
static struct { void *session; char name[72]; } session_of[SESSIONS];
static unsigned sessions_known;

static const char *
name_of_session(void *session)
{
	for (unsigned i = 0; i < sessions_known; i++) {
		if (session_of[i].session == session) {
			return session_of[i].name;
		}
	}
	return "(a session made elsewhere)";
}

static void
remember_session(void *session, const char *name)
{
	if (session != NULL && name != NULL && sessions_known < SESSIONS) {
		session_of[sessions_known].session = session;
		snprintf(session_of[sessions_known].name, sizeof(session_of[0].name), "%s", name);
		sessions_known++;
	}
}

static void
say_rich(const char *what, const char *name, void *error)
{
	char *why = error != NULL && rich_desc != NULL ? rich_desc(error) : NULL;

	say("hakios: store: xpc session %s%s%s: %s\n", what, name != NULL ? " " : "", name != NULL ? name : "",
	    why != NULL ? why : (error != NULL ? "an error with no description" : "ok"));
	free(why);
}

static void *
new_session_create(const char *name, void *queue, unsigned long long flags, void **error)
{
	void *session = old_session_create(name, queue, flags, error);

	remember_session(session, name);
	say_rich(session != NULL ? "made for" : "NOT made for", name, error != NULL ? *error : NULL);
	return session;
}

static bool
new_session_activate(void *session, void **error)
{
	bool ok = old_session_activate(session, error);

	say_rich(ok ? "activated" : "NOT activated", name_of_session(session), error != NULL ? *error : NULL);
	return ok;
}

/*
 * The message itself, not only whether it was answered. The session to jetpackassetd is made, activated
 * and answered -- and the App Store still asks the network for nothing, so what matters is what JetEngine
 * asked its daemon for and what the daemon said back. xpc_copy_description prints a whole dictionary;
 * only the first part of each is kept, which is where the keys are.
 */
static void
say_xpc(const char *what, void *object)
{
	char *text = object != NULL && xpc_desc != NULL ? xpc_desc(object) : NULL;

	say("hakios: store: xpc %s %.700s\n", what, text != NULL ? text : "(nothing)");
	free(text);
}

static void *
new_session_send_sync(void *session, void *message, void **error)
{
	void *reply;

	say("hakios: store: a sync message to %s\n", name_of_session(session));
	say_xpc("asks:", message);
	reply = old_session_send_sync(session, message, error);
	say_rich(reply != NULL ? "answered a sync message" : "gave NO answer to a sync message",
	    name_of_session(session), error != NULL ? *error : NULL);
	say_xpc("answer:", reply);
	return reply;
}

/* xpc_session_send_message takes the session and the message and RETURNS the rich error (nil when it went). */
static void *(*old_session_send)(void *, void *);

static void *
new_session_send(void *session, void *message)
{
	void *error;

	say("hakios: store: a one-way message to %s\n", name_of_session(session));
	say_xpc("sends one way:", message);
	error = old_session_send(session, message);
	say_rich(error == NULL ? "sent one way" : "FAILED to send one way", name_of_session(session), error);
	return error;
}

/*
 * EVERY os_log MESSAGE, WHATEVER ITS LEVEL (exec_env=NAME:OS_ACTIVITY_DT_MODE=1;HAKIOS_ALL_LOGS=1).
 * Apple's own lines are the best account of what the store is waiting for, but most of them are debug or
 * info, which os_log drops before formatting. The documented switch for that is a plist under
 * /Library/Preferences/Logging/Subsystems (root26-darwin.sh LOGGING_DEBUG=...), and on this port it changes
 * nothing: the App Store said the same twenty-four lines with com.apple.AppleMediaServices, com.apple.JetEngine
 * and four more set to Debug as without them, so whatever reads those files here does not reach the process.
 * This asks the question in the process instead: os_log_type_enabled is what every os_log(_impl) call asks
 * before it formats anything, so answering yes turns all of them on. Nothing else is changed -- where the
 * line goes is still OS_ACTIVITY_DT_MODE's business (standard error, which libhakios-console.dylib puts on
 * the console). Off unless asked for: the answer is a flood.
 */
static bool
all_logs_enabled(__unused void *log, __unused unsigned char type)
{
	return true;
}

/*
 * ... and the message itself, said by us rather than by os_log. Answering "enabled" turned nothing on in
 * the log of run 193452 -- twelve lines from the App Store, half of what the same run says without it --
 * so where the formatted line goes in OS_ACTIVITY_DT_MODE is not a question this process answers. What
 * every enabled message does pass through is _os_log_impl, and its fourth argument is the format string,
 * which names the message even with its arguments left out ("Skipping GUID parameter addition as bag was
 * not provided" is a format string with no arguments at all). So both are hooked: everything is enabled,
 * and every message that then reaches _os_log_impl has its format put on the console by us.
 */
static void (*old_log_impl)(void *, void *, unsigned char, const char *, unsigned char *, unsigned int);

/*
 * NOT USED, and kept only so the next person does not try it again: hooking _os_log_impl kills the App
 * Store. With every message said on the console the app passed no launch milestone and ended by signal 9
 * (run 194658); with only the messages whose format holds one of the words below -- a handful of lines --
 * it died the same way, a second after our constructor and before the next line of ours (run 195845), so
 * what kills it is the hook on that function, not the writing. A function this far down (every framework
 * in the process logs through it, and so does whatever ElleKit's own hooking does) is not one to replace.
 */
static const char *const log_words[] = { "et", "ack", "age", "oad", "ag", "torefront", "aemon", "oken",
	"atalog", "ccount", "etch", "rror", "ail" };

static void
new_log_impl(void *dso, void *log, unsigned char type, const char *format, unsigned char *buffer,
    unsigned int size)
{
	static unsigned said;

	if (format != NULL && said < 4000) {
		for (unsigned i = 0; i < sizeof(log_words) / sizeof(log_words[0]); i++) {
			if (strstr(format, log_words[i]) != NULL) {
				said++;
				say("hakios: oslog %u: %.200s\n", (unsigned)type, format);
				break;
			}
		}
	}
	old_log_impl(dso, log, type, format, buffer, size);
}

static void
hook_all_logs(MSHookFunction_t hook)
{
	const char *wanted = knob("HAKIOS_ALL_LOGS");
	static bool (*old_type_enabled)(void *, unsigned char);
	void *target = dlsym(RTLD_DEFAULT, "os_log_type_enabled");
	void *impl = dlsym(RTLD_DEFAULT, "_os_log_impl");

	if (wanted == NULL || *wanted == '0' || hook == NULL || target == NULL) {
		return;
	}
	(void)impl, (void)new_log_impl, (void)old_log_impl, (void)log_words;
	hook(target, (void *)all_logs_enabled, (void **)&old_type_enabled);
	say("hakios: store: every os_log level is answered enabled\n");
}

/*
 * ... and the asynchronous one, which is the one that matters. A JetPack is asked for with a reply handler,
 * not synchronously: with only the sync and one-way sends named, the App Store looked as though it said
 * nothing to its daemon after "initialize" -- a silence that was ours, not the app's.
 */
static void (*old_session_send_async)(void *, void *, void *);

static void
new_session_send_async(void *session, void *message, void *handler)
{
	/*
	 * The message only. Unlike the one-way send, this one does not hand back a rich error: taking what
	 * it leaves in x0 for one and asking it to describe itself killed the App Store at the first
	 * message (run 201010, a fault in xpc_rich_error_copy_description).
	 */
	say("hakios: store: a message with a reply handler to %s\n", name_of_session(session));
	say_xpc("asks with a reply handler:", message);
	old_session_send_async(session, message, handler);
}

/* the other creator by name: an XPC service rather than a Mach service */
static void *(*old_session_create_service)(const char *, void *, unsigned long long, void **);

static void *
new_session_create_service(const char *name, void *queue, unsigned long long flags, void **error)
{
	void *session = old_session_create_service(name, queue, flags, error);

	remember_session(session, name);
	say_rich(session != NULL ? "made for the XPC service" : "NOT made for the XPC service", name,
	    error != NULL ? *error : NULL);
	return session;
}

/*
 * ... and the one Swift's XPCSession really uses, which wraps a connection that was made first:
 * _xpc_session_create_from_connection_4SWIFT. The App Store makes its first session to jetpackassetd by
 * name (that is the "prewarm"), and the session that carries "initialize" comes from here -- without this
 * the message that matters is sent to "a session made elsewhere" and there is no telling whether the Jet
 * daemon was told anything at all. The connection knows its own service name (xpc_connection_get_name).
 */
static void *(*old_session_from_connection)(void *, void *, void *, void *);
static const char *(*connection_name)(void *);

static void *
new_session_from_connection(void *connection, void *a, void *b, void *c)
{
	void *session = old_session_from_connection(connection, a, b, c);
	const char *name = connection != NULL && connection_name != NULL ? connection_name(connection) : NULL;

	remember_session(session, name != NULL ? name : "(a connection with no name)");
	say("hakios: store: xpc session made from a connection to %s\n", name != NULL ? name : "(no name)");
	return session;
}

/*
 * FAIRPLAY, ANSWERED BEFORE ANYTHING CAN BLOCK IN IT.
 *
 * -[AMSUserAgent _sharedComponentFairPlayDeviceType] is already answered by this library, and on 16.5
 * that was enough. On iOS 27 it is not: a thread of the App Store sits in that method's dispatch_once
 * block -- _AMSFairPlayGetDeviceType -> the obfuscated FairPlay code -> mach_msg, with no reply ever --
 * in every report of every run, including the runs where our answer was installed and used. The two are
 * not a contradiction: the first call happens before a method of ours exists, and that one thread stays
 * in there for the life of the process while everything the store wants to ask queues behind it. (16.5
 * found the same wall from the other side: fairplayd.H2 starts, decides there is nothing here and leaves
 * at once, 34 times in six minutes -- components/apple-id/first-generation/inject_fairplay.c, which is worth reading before
 * anyone writes a kernel driver for this. It never opens an IOKit user client at all.)
 *
 * So the C function is replaced as well, from the constructor, before a line is said or a method is
 * looked at. It answers "no" the way the real one does when FairPlay has nothing for it -- a non-zero
 * status, with its out-parameters untouched -- and never asks the daemon. HAKIOS_NO_FAIRPLAY_HOOK=1 in
 * /var/hakios-store.conf puts it back, to see the stall again without building anything.
 */
static int (*old_fp_get_device_type)(void *);

static int
new_fp_get_device_type(void *out)
{
	static bool said;

	(void)out;
	if (!said) {
		said = true;
		say("hakios: store: AMSFairPlayGetDeviceType answered an error, without asking FairPlay\n");
	}
	return 1;
}

static MSHookFunction_t
substrate_hook(void)
{
	void *substrate = dlopen("/var/jb/usr/lib/libsubstrate.dylib", RTLD_NOW);

	if (substrate == NULL) {
		substrate = dlopen("/var/jb/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate", RTLD_NOW);
	}
	return substrate != NULL ? (MSHookFunction_t)dlsym(substrate, "MSHookFunction") : NULL;
}

/*
 * The exception nobody caught, named before the process dies. amsaccountsd aborts with signal 6 once it
 * is given an account to work with, and an abort out of a daemon says nothing at all by itself -- the
 * kernel's backtrace stops at abort(). (The applications have this already, in darwin/userland/uncaught.c;
 * a daemon has no library of ours but this one.)
 */
static void (*previous_uncaught)(id);

static void
uncaught_here(id exception)
{
	const char *name = NULL, *reason = NULL;
	char one[80] = "?", two[300] = "?";
	id n = ((id (*)(id, SEL))objc_msgSend)(exception, sel_registerName("name"));
	id r = ((id (*)(id, SEL))objc_msgSend)(exception, sel_registerName("reason"));

	name = n != NULL ? text_of(n, one, sizeof(one)) : "?";
	reason = r != NULL ? text_of(r, two, sizeof(two)) : "?";
	say("hakios: store: UNCAUGHT %s: %.280s\n", name, reason);
	if (previous_uncaught != NULL) {
		previous_uncaught(exception);
	}
}

static void
name_uncaught_exceptions(void)
{
	void (*set_handler)(void (*)(id)) = (void (*)(void (*)(id)))dlsym(RTLD_DEFAULT,
	    "objc_setUncaughtExceptionHandler");

	if (set_handler != NULL) {
		set_handler(uncaught_here);
	}
}

/*
 * THE ACCOUNT TYPES, BUILT BY ACCOUNTSD'S OWN CODE (HAKIOS_ACCOUNT_TYPES=1, accountsd only).
 *
 * accountsd knows how to make every account type this phone is missing -- ACDDatabaseInitializer has an
 * -_addiTunesStoreAccountType, and ninety more beside it -- and it never runs them here: ZACCOUNTTYPE is
 * empty on every boot, which is why no client is ever authorized for a type and why there is no account.
 * The steps are driven by -[ACDDatabaseInitializer updateDefaultContentIfNecessary:], and the hook below
 * is never entered -- four starts of the daemon in a boot, installed in each. That is not the daemon being
 * quiet on this port: NOTHING CALLS IT AT ALL. Every B and BL in AccountsDaemon's __text was scanned for
 * the selector stub _objc_msgSend$updateDefaultContentIfNecessary: (0x2546f4560 in the extracted image)
 * and there is not one branch to it. The method is there, the ninety types it would make are there, and
 * no code in that framework asks.
 *
 * So the account types are not made by a daemon we fail to run -- they are a RESTORE ARTEFACT. A real
 * iPhone gets /var/mobile/Library/Accounts/Accounts3.sqlite with the types already in it, and this port
 * builds /var afresh at every boot, so it never has them. Which makes the way out plain enough: call that
 * initializer ourselves from inside accountsd (-[ACDServer createDatabaseConnection] makes the connection
 * it wants, -[ACDDatabaseInitializer initWithDatabaseConnection:] takes it, and -_addiTunesStoreAccountType
 * is one of its own methods), let it write the database, and from then on every answer downstream is
 * Apple's own data rather than ours. That is the next piece of work, and it is the first one in weeks that
 * does not end in a guess.
 */
static void (*old_update_default_content)(id, SEL, void *);

static void
new_update_default_content(id self, SEL cmd, void *argument)
{
	char text[120] = "?";

	say("hakios: store: updateDefaultContentIfNecessary: %p (%.80s)\n", argument,
	    argument != NULL ? text_of((id)argument, text, sizeof(text)) : "nil");
	old_update_default_content(self, cmd, argument);
	say("hakios: store: updateDefaultContentIfNecessary: came back\n");
}

/*
 * ... and then made, by that same code (HAKIOS_MAKE_TYPES=1, accountsd only).
 *
 * Nothing calls updateDefaultContentIfNecessary: in this framework at all, so the types are never made
 * here and the whole store is refused for want of one. They are Apple's rows, not ours, so Apple's code
 * writes them: a connection of its own (-[ACDDatabaseConnection init], which finds the database the
 * daemon uses), the initializer around it, its own -_addiTunesStoreAccountType, and its own save. What
 * comes out is the row a restored iPhone is handed, with whatever access keys and owning bundle it sets,
 * rather than the five columns a person would think to fill in by hand.
 */
/*
 * The daemon's own connection, remembered as it hands it out. Nothing is changed: the accessor is called
 * and its answer returned untouched, which is the one thing this library does in accountsd beyond looking.
 */
static id remembered_connection;
static id (*old_database_connection)(id, SEL);

static id
new_database_connection(id self, SEL cmd)
{
	id connection = old_database_connection != NULL ? old_database_connection(self, cmd) : NULL;

	if (connection != NULL && remembered_connection == NULL) {
		remembered_connection = connection;
		say("hakios: store: accountsd's own database connection is %p\n", (void *)connection);
	}
	return connection;
}

/*
 * ... and the store that owns it. -[ACDAccountStore databaseConnection] is never called by the daemon
 * itself (the hook on it sat through four restarts and a client's question without firing), so the
 * connection is reached the other way round: remember a store object as it serves a request, and ask
 * THAT for its connection, which is what the accessor is for. -_allAccounts_sync is the method a client
 * asking "what accounts are there" lands in, which is exactly the question the App Store asks at launch.
 */
static id remembered_store;
static id (*old_all_accounts_sync)(id, SEL);

static id
new_all_accounts_sync(id self, SEL cmd)
{
	if (remembered_store == NULL) {
		remembered_store = self;
		say("hakios: store: accountsd's own account store is %p\n", (void *)self);
	}
	return old_all_accounts_sync != NULL ? old_all_accounts_sync(self, cmd) : NULL;
}

/*
 * ... and, since neither door to the connection is used, the connection itself. Any instance method of
 * ACDDatabaseConnection has the connection as its receiver, so hooking one that the daemon certainly
 * calls -- -managedObjectContext, which every query of its own goes through -- hands over the object
 * without having to know which of accountsd's many paths reached it. Pass-through: the original is
 * called and its answer returned.
 */
static id (*old_managed_object_context)(id, SEL);

/*
 * ... and the work is done HERE, inside the hook, the first time accountsd itself asks its connection for
 * a context. Everything about that connection is confined to the daemon's own queue: a connection of our
 * own never returns, -performBlockAndWait: from our thread never returns, and even asking the connection
 * for its context from our thread never returns. Inside this hook none of that is a question -- the
 * daemon called us, so we are already on its queue, with its context, between two pieces of its own work.
 * It happens once, and the flag is set before the first message so that the initializer's own use of this
 * same accessor does not come back round into it.
 */
static id
new_managed_object_context(id self, SEL cmd)
{
	static bool making;
	id context;

	if (remembered_connection == NULL) {
		remembered_connection = self;
		say("hakios: store: accountsd's database connection, caught at work: %p\n", (void *)self);
	}
	context = old_managed_object_context != NULL ? old_managed_object_context(self, cmd) : NULL;
	if (!making && knob("HAKIOS_MAKE_TYPES") != NULL && *knob("HAKIOS_MAKE_TYPES") != '0') {
		Class init_class = objc_getClass("ACDDatabaseInitializer");
		id initializer, error = NULL;
		BOOL ok;

		making = true;
		initializer = init_class != Nil ? ((id (*)(id, SEL))objc_msgSend)((id)init_class,
		    sel_registerName("alloc")) : NULL;
		initializer = initializer != NULL ? ((id (*)(id, SEL, id))objc_msgSend)(initializer,
		    sel_registerName("initWithDatabaseConnection:"), self) : NULL;
		if (initializer == NULL) {
			say("hakios: store: no initializer on the daemon's own queue\n");
			return context;
		}
		say("hakios: store: on accountsd's own queue now; asking for the iTunes Store type\n");
		((void (*)(id, SEL))objc_msgSend)(initializer, sel_registerName("_addiTunesStoreAccountType"));
		ok = ((BOOL (*)(id, SEL, id *))objc_msgSend)(self, sel_registerName("saveWithError:"), &error);
		{
			char text[200] = "none";

			say("hakios: store: the iTunes Store account type was made by accountsd's own code; "
			    "saved %s (%.150s)\n", ok ? "yes" : "NO",
			    error != NULL ? text_of(error, text, sizeof(text)) : text);
		}
	}
	return context;
}

static void
remember_database_connection(void)
{
	Class conn_class = objc_getClass("ACDDatabaseConnection");
	Method context = conn_class != Nil ? class_getInstanceMethod(conn_class,
	    sel_registerName("managedObjectContext")) : NULL;
	Class store_class = objc_getClass("ACDAccountStore");
	Method m = store_class != Nil ? class_getInstanceMethod(store_class,
	    sel_registerName("databaseConnection")) : NULL;
	Method all = store_class != Nil ? class_getInstanceMethod(store_class,
	    sel_registerName("_allAccounts_sync")) : NULL;

	if (m != NULL) {
		old_database_connection = (id (*)(id, SEL))method_getImplementation(m);
		method_setImplementation(m, (IMP)new_database_connection);
	}
	if (all != NULL) {
		old_all_accounts_sync = (id (*)(id, SEL))method_getImplementation(all);
		method_setImplementation(all, (IMP)new_all_accounts_sync);
	}
	if (context != NULL) {
		old_managed_object_context = (id (*)(id, SEL))method_getImplementation(context);
		method_setImplementation(context, (IMP)new_managed_object_context);
	}
	say("hakios: store: watching for accountsd's database connection (accessor %s, store %s, context %s)\n",
	    m != NULL ? "hooked" : "MISSING", all != NULL ? "hooked" : "MISSING",
	    context != NULL ? "hooked" : "MISSING");
}

static void
make_account_types(void)
{
	const char *wanted = knob("HAKIOS_MAKE_TYPES");
	Class conn_class = objc_getClass("ACDDatabaseConnection");
	Class init_class = objc_getClass("ACDDatabaseInitializer");
	id connection, initializer, error = NULL;
	BOOL saved;

	/* said whatever happens: a switch that was never seen and a switch that was seen and declined read
	 * the same in a log that says nothing, and this one has cost two runs already. */
	say("hakios: store: make the account types? %s (connection class %s, initializer %s)\n",
	    wanted != NULL ? wanted : "(the switch is not set)", conn_class != Nil ? "there" : "MISSING",
	    init_class != Nil ? "there" : "MISSING");
	if (wanted == NULL || *wanted == '0' || conn_class == Nil || init_class == Nil) {
		return;
	}
	/*
	 * Said before the connection is made, not after -- and the answer is in: "asking accountsd's own
	 * class for a database connection" is the last line this function ever prints. -[ACDDatabaseConnection
	 * init] DOES NOT COME BACK. The daemon lives on (it is our watcher thread that stops there), so it is
	 * a wait, not a fault: a second connection to a store the daemon already holds open.
	 *
	 * So the way in is not to make one. -[ACDAccountStore databaseConnection] is the daemon's own, and
	 * hooking that accessor purely to remember the object it returns costs nothing and gives the one
	 * connection that is certainly usable; the initializer takes it
	 * (-[ACDDatabaseInitializer initWithDatabaseConnection:]) and its own -_addiTunesStoreAccountType
	 * writes the row. That is the next piece of work, and it is a hook and three messages.
	 */
	/*
	 * The daemon's own, not one of ours: -[ACDDatabaseConnection init] never comes back here (it waits
	 * on a store accountsd already holds open), so the accessor is hooked instead and this waits for
	 * the daemon to hand one out in its own time.
	 */
	for (int i = 0; i < 60 && remembered_connection == NULL && remembered_store == NULL; i++) {
		sleep(1);
	}
	connection = remembered_connection;
	if (connection == NULL && remembered_store != NULL) {
		connection = ((id (*)(id, SEL))objc_msgSend)(remembered_store,
		    sel_registerName("databaseConnection"));
		say("hakios: store: asked accountsd's own store for its connection: %p\n", (void *)connection);
	}
	if (connection == NULL) {
		say("hakios: store: accountsd never handed out a database connection: no account types made\n");
		return;
	}
	say("hakios: store: using accountsd's own connection (%p); now the initializer\n", (void *)connection);
	initializer = ((id (*)(id, SEL))objc_msgSend)((id)init_class, sel_registerName("alloc"));
	initializer = initializer != NULL ? ((id (*)(id, SEL, id))objc_msgSend)(initializer,
	    sel_registerName("initWithDatabaseConnection:"), connection) : NULL;
	if (initializer == NULL) {
		say("hakios: store: no initializer: no account types made\n");
		return;
	}
	/* a line between every step: with one line before the three of them and one after, a stop anywhere
	 * in the middle reads the same */
	say("hakios: store: the initializer is made (%p); asking it for the iTunes Store type\n",
	    (void *)initializer);
	/*
	 * ON THE CONTEXT'S OWN QUEUE. -_addiTunesStoreAccountType never came back when it was called from
	 * our thread: it is Core Data work, and accountsd's context is a private-queue one, so a call from
	 * anywhere else waits for a queue it is not on. -performBlockAndWait: puts the work where the
	 * context lives and returns when it is done, which is also what makes the save safe below.
	 */
	{
		id context = ((id (*)(id, SEL))objc_msgSend)(connection,
		    sel_registerName("managedObjectContext"));

		if (context == NULL) {
			say("hakios: store: the connection has no managed object context: no account types made\n");
			return;
		}
		/*
		 * ASYNCHRONOUSLY. -performBlockAndWait: did not come back either, which says the queue is
		 * busy with something of the daemon's own, not that the call is wrong; so the work is handed
		 * to the queue and this thread lets go. The whole of it -- the type and the save -- is inside
		 * the block, because both have to happen where the context lives, and the block says how it
		 * went when it gets there.
		 */
		((void (*)(id, SEL, void (^)(void)))objc_msgSend)(context,
		    sel_registerName("performBlock:"), ^{
			id save_error = NULL;
			BOOL ok;

			((void (*)(id, SEL))objc_msgSend)(initializer,
			    sel_registerName("_addiTunesStoreAccountType"));
			ok = ((BOOL (*)(id, SEL, id *))objc_msgSend)(connection,
			    sel_registerName("saveWithError:"), &save_error);
			{
				char text[200] = "none";

				say("hakios: store: the iTunes Store account type was made by accountsd's own "
				    "code; saved %s (%.150s)\n", ok ? "yes" : "NO",
				    save_error != NULL ? text_of(save_error, text, sizeof(text)) : text);
			}
		});
	}
	say("hakios: store: the work is on the context's queue; the block will say how it went\n");
	return;
	saved = ((BOOL (*)(id, SEL, id *))objc_msgSend)(connection, sel_registerName("saveWithError:"), &error);
	{
		char text[200] = "none";

		say("hakios: store: the iTunes Store account type was made by accountsd's own code; saved %s (%.150s)\n",
		    saved ? "yes" : "NO", error != NULL ? text_of(error, text, sizeof(text)) : text);
	}
}

/*
 * accountsd's answer to appstored is "Failed to create/fetch the local account. error = { Error
 * domain=com.apple.accounts, code=9 }" -- code 9 is the refusal, not the absence. The daemon decides it
 * through ACDAccessPluginManager, and what the decision comes down to is one BOOL:
 * -[ACDClientAuthorization isGranted]. With HAKIOS_GRANT_ACCOUNT_ACCESS=1 that answer is YES here.
 *
 * This process is a watcher by default (see the constructor), so the hook is put in by hand rather than
 * through the table, the same way the database connection is.
 */
static BOOL (*old_is_granted)(id, SEL);

static BOOL
new_is_granted(id self, SEL cmd)
{
	static int told;
	BOOL real = old_is_granted != NULL ? old_is_granted(self, cmd) : NO;

	if (told < 3) {
		told++;
		say("hakios: store: a client's authorization said %s; YES is answered\n",
		    real ? "YES" : "NO");
	}
	return YES;
}

/*
 * ... and isGranted is not the one that is asked. The refusal names itself in appstored's log --
 * "Attempted to register account monitor for types client is not authorized to access" -- and the class
 * that refuses is ACDAccountStoreFilter, whose private gates are these four. All of them answer YES.
 */
static BOOL (*old_permitted_type)(id, SEL, void *);
static BOOL (*old_permitted_account)(id, SEL, void *);
static BOOL (*old_granted_client)(id, SEL, void *, void *);
static BOOL (*old_granted_bundle)(id, SEL, void *, void *);

static BOOL
new_permitted_type(id self, SEL cmd, void *identifier)
{
	static int told;

	(void)self, (void)cmd, (void)identifier;
	if (told < 3) {
		told++;
		say("hakios: store: a client asked to reach an account type; YES is answered\n");
	}
	return YES;
}

static BOOL
new_permitted_account(id self, SEL cmd, void *account)
{
	(void)self, (void)cmd, (void)account;
	return YES;
}

static BOOL
new_granted_client(id self, SEL cmd, void *client, void *type)
{
	(void)self, (void)cmd, (void)client, (void)type;
	return YES;
}

static BOOL
new_granted_bundle(id self, SEL cmd, void *bundle, void *type)
{
	(void)self, (void)cmd, (void)bundle, (void)type;
	return YES;
}

/*
 * ... and the reason isGranted is never reached: the manager answers nil for a client it has no row for,
 * and nil is already the refusal. -[ACDClientAuthorizationManager authorizationForClient:accountType:]
 * and its by-identifier twin make one here instead -- ACDClientAuthorization has initForClient:, and
 * whatever it holds, the isGranted above answers YES.
 */
static id (*old_auth_for_client)(id, SEL, void *, void *);
static id (*old_auth_for_client_id)(id, SEL, void *, void *);

static id
an_authorization_for(id client, id answer)
{
	Class cls = objc_getClass("ACDClientAuthorization");
	id made;
	static int told;

	if (answer != NULL || cls == Nil || client == NULL) {
		return answer;
	}
	made = ((id (*)(Class, SEL))objc_msgSend)(cls, sel_registerName("alloc"));
	made = made != NULL ? ((id (*)(id, SEL, id))objc_msgSend)(made, sel_registerName("initForClient:"),
	    client) : NULL;
	if (made != NULL && told < 3) {
		told++;
		say("hakios: store: this client had no authorization row; one is made for it\n");
	}
	return made;
}

static id
new_auth_for_client(id self, SEL cmd, void *client, void *type)
{
	id answer = old_auth_for_client != NULL ? old_auth_for_client(self, cmd, client, type) : NULL;

	return an_authorization_for((id)client, answer);
}

static id
new_auth_for_client_id(id self, SEL cmd, void *client, void *identifier)
{
	id answer = old_auth_for_client_id != NULL ? old_auth_for_client_id(self, cmd, client, identifier)
	    : NULL;

	return an_authorization_for((id)client, answer);
}

/*
 * ... and the one that is actually asked. Beside the four private gates there is a PUBLIC one on the
 * same class -- isClientEntitledToAccessAccountTypeWithIdentifier: -- and under it the daemon's general
 * question about a client, -[ACDClient hasEntitlement:]. Those two are the shape of "client is not
 * authorized to access".
 */
static BOOL (*old_entitled_type)(id, SEL, void *);
static BOOL (*old_has_entitlement)(id, SEL, void *);

static BOOL
new_entitled_type(id self, SEL cmd, void *identifier)
{
	static int told;

	(void)self, (void)cmd, (void)identifier;
	if (told < 3) {
		told++;
		say("hakios: store: a client was asked for its entitlement to an account type; YES\n");
	}
	return YES;
}

static BOOL
new_has_entitlement(id self, SEL cmd, void *entitlement)
{
	static int told;
	BOOL real = old_has_entitlement != NULL ? old_has_entitlement(self, cmd, entitlement) : NO;

	if (!real && told < 4) {
		told++;
		say("hakios: store: a client lacked an entitlement; YES is answered\n");
	}
	return YES;
}

static void
one_grant(Class cls, const char *selector, IMP replacement, void *original)
{
	Method m = cls != Nil ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

	if (m == NULL) {
		say("hakios: store: %s is not there to replace\n", selector);
		return;
	}
	*(IMP *)original = method_getImplementation(m);
	method_setImplementation(m, replacement);
}

static void new_filter_accounts(id self, SEL cmd, void *type, void *options, void *completion);
static void (*old_filter_accounts)(id, SEL, void *, void *, void *);

static void
grant_account_access(void)
{
	const char *on = knob("HAKIOS_GRANT_ACCOUNT_ACCESS");
	Class authorization = objc_getClass("ACDClientAuthorization");
	Class filter = objc_getClass("ACDAccountStoreFilter");
	Method m = authorization != Nil ? class_getInstanceMethod(authorization,
	    sel_registerName("isGranted")) : NULL;

	if (on == NULL || *on == '0') {
		return;
	}
	if (m != NULL) {
		old_is_granted = (BOOL (*)(id, SEL))method_getImplementation(m);
		method_setImplementation(m, (IMP)new_is_granted);
	}
	one_grant(filter, "_isClientPermittedToAccessAccountTypeWithIdentifier:", (IMP)new_permitted_type,
	    &old_permitted_type);
	one_grant(filter, "_isClientPermittedToAccessAccount:", (IMP)new_permitted_account,
	    &old_permitted_account);
	one_grant(filter, "_accessGrantedForClient:onAccountTypeID:", (IMP)new_granted_client,
	    &old_granted_client);
	one_grant(filter, "_accessGrantedForBundleID:onAccountTypeID:", (IMP)new_granted_bundle,
	    &old_granted_bundle);
	one_grant(filter, "isClientEntitledToAccessAccountTypeWithIdentifier:", (IMP)new_entitled_type,
	    &old_entitled_type);
	one_grant(objc_getClass("ACDClient"), "hasEntitlement:", (IMP)new_has_entitlement,
	    &old_has_entitlement);
	{
		Class manager = objc_getClass("ACDClientAuthorizationManager");

		if (knob("HAKIOS_BYPASS_ACCOUNT_FILTER") != NULL
		    && *knob("HAKIOS_BYPASS_ACCOUNT_FILTER") != '0') {
			one_grant(objc_getClass("ACDAccountStoreFilter"),
			    "accountsWithAccountType:options:completion:", (IMP)new_filter_accounts,
			    &old_filter_accounts);
			say("hakios: store: the account filter forwards to its backing store\n");
		}
		one_grant(manager, "authorizationForClient:accountType:", (IMP)new_auth_for_client,
		    &old_auth_for_client);
		one_grant(manager, "authorizationForClient:accountTypeWithIdentifier:",
		    (IMP)new_auth_for_client_id, &old_auth_for_client_id);
	}
	say("hakios: store: every client's authorization to reach an account is answered YES\n");
}

static void
watch_account_types(void)
{
	const char *wanted = knob("HAKIOS_ACCOUNT_TYPES");
	Class initializer = objc_getClass("ACDDatabaseInitializer");
	Method m = initializer != Nil ? class_getInstanceMethod(initializer,
	    sel_registerName("updateDefaultContentIfNecessary:")) : NULL;

	if (wanted == NULL || *wanted == '0' || m == NULL) {
		return;
	}
	old_update_default_content = (void (*)(id, SEL, void *))method_getImplementation(m);
	method_setImplementation(m, (IMP)new_update_default_content);
	say("hakios: store: watching what accountsd is told about its default content\n");
}

/*
 * appstored dies the same death 1144 times in a run: signal 11 at 0x38, in std::mutex::lock, under
 * AppleMediaServicesKitInternal's own logging call (sub_1ab7b2244 on 24B119). The outermost frame names
 * what is being done -- AMSAccount.init(ACAccount) throws -- so the account is refused and the daemon
 * dies WRITING DOWN why: the block that formats the line reads [x0+0x60], which is nil, and locks the
 * mutex at +0x38 of nothing. Every restart of appstored takes the App Store's XPC connection with it
 * (NSCocoaErrorDomain 4099, 1613 times), which is why the store never gets past "Loading...".
 *
 * The function's own first branch is "if the first argument is nil, return 0": answering 0 is what the
 * code does when there is nothing to log, so that is what is answered here. The error is still made and
 * still thrown -- our NSError hooks name it -- only AMS's own line about it is lost.
 */
static long (*old_ams_log)(void *, void *, void *, void *, void *, void *);

static long
new_ams_log(void *a, void *b, void *c, void *d, void *e, void *f)
{
	static bool said;

	(void)a, (void)b, (void)c, (void)d, (void)e, (void)f;
	if (!said) {
		said = true;
		say("hakios: store: AMS's own logging call is answered 0, the way it answers itself when "
		    "there is nothing to log\n");
	}
	return 0;
}

static void
hook_ams_log(void)
{
	static bool done, said;
	const char *on = knob("HAKIOS_MUTE_AMS_LOG");
	MSHookFunction_t hook = substrate_hook();
	/*
	 * Only once AppleMediaServicesKitInternal is in the process: its Swift symbols are exported, so
	 * dlsym of one of them is both the question "is it here" and a second opinion on the slide.
	 */
	void *anchor = dlsym(RTLD_DEFAULT, "$s29AppleMediaServicesKitInternal0B7AccountVyACSo9ACAccountCKcfC");
	const unsigned int *target;

	if (done || on == NULL || *on == '0' || hook == NULL || slide == 0 || anchor == NULL) {
		return;
	}
	target = (const unsigned int *)(0x1ab7b2244UL + slide);
	/*
	 * ... and only if the address still holds that function. Three words of it are the signature:
	 * pacibsp; sub sp, sp, #0x80; stp x20, x19, [sp, #0x60]. A build whose cache differs stops here
	 * instead of replacing whatever else is at the address.
	 */
	if (target[0] != 0xd503237fu || target[1] != 0xd10203ffu || target[2] != 0xa9064ff4u) {
		if (!said) {
			said = true;
			say("hakios: store: AMS's logging call is NOT where this build expects it "
			    "(%p holds %08x %08x %08x); nothing replaced\n", (void *)target,
			    target[0], target[1], target[2]);
		}
		done = true;
		return;
	}
	done = true;
	hook((void *)target, (void *)new_ams_log, (void **)&old_ams_log);
	say("hakios: store: AMS's logging call (%p) is replaced; appstored no longer dies writing down "
	    "why an account was refused\n", (void *)target);
}

/*
 * jetpackassetd does nothing at all before the device's first unlock: its own classes are called
 * DefaultBeforeFirstUnlockChecker and beforeFirstUnlockChecker, it listens for
 * com.apple.mobile.keybagd.first_unlock, and it asks MKBDeviceUnlockedSinceBoot(). Nothing of its bag
 * machinery is ever entered in a run of ours -- not _bagAccountForAccountMediaType:, not one bag key --
 * and the App Store's content is a JetPack that only this daemon fetches (URLJetPackAssetFetcher takes
 * its downloadsBaseURL from the bag). So the store's "Loading..." and this question may be the same
 * thing. What the phone answers is printed in every report; HAKIOS_UNLOCKED=1 answers 1 instead.
 */
static int (*old_unlocked_since_boot)(void);

static int
new_unlocked_since_boot(void)
{
	static bool said;
	int real = old_unlocked_since_boot != NULL ? old_unlocked_since_boot() : -1;

	if (!said) {
		said = true;
		say("hakios: store: MKBDeviceUnlockedSinceBoot answers %d by itself; 1 is answered instead\n",
		    real);
	}
	return 1;
}

static int
unlocked_since_boot_now(void)
{
	int (*f)(void) = (int (*)(void))(old_unlocked_since_boot != NULL ? (void *)old_unlocked_since_boot
	    : dlsym(RTLD_DEFAULT, "MKBDeviceUnlockedSinceBoot"));

	return f != NULL ? f() : -1;
}

static void
hook_first_unlock(void)
{
	static bool done;
	const char *on = knob("HAKIOS_UNLOCKED");
	MSHookFunction_t hook = substrate_hook();
	void *target = dlsym(RTLD_DEFAULT, "MKBDeviceUnlockedSinceBoot");

	if (done || hook == NULL || target == NULL || on == NULL || *on == '0') {
		return;
	}
	done = true;
	hook(target, (void *)new_unlocked_since_boot, (void **)&old_unlocked_since_boot);
	say("hakios: store: MKBDeviceUnlockedSinceBoot (%p) is replaced: this process is told the device "
	    "has been unlocked since boot\n", target);
}

/*
 * TRACE THE BOOTSTRAP. Static read of AppStoreKit (cache 24B119): the App Store shows
 * DefaultBootstrapLoadingView ("Loading...") until ASKBootstrapV2.start() (@0x20b45c080) resolves its
 * Promise<BaseObjectGraph>, which it does only after a list of JetEngine prerequisites all finish; each
 * phase that finishes calls -[BootstrapPendingClosuresHandler didComplete:bootstrapPhase:] (@0x20b5d6b80).
 * The store is stuck on Loading and never asks for its own bag or amp-api, so one prerequisite never
 * finishes. didComplete is a class method (self in x20, callee-saved), so logging each completed phase and
 * calling through is safe; the sequence stops right after the last phase that completes, which names the
 * stall. HAKIOS_TRACE_BOOTSTRAP=1, AppStore only. Addresses are unslid cache vaddrs + slide.
 */
/*
 * THE DECISIVE TEST. ASKBootstrapV2.prerequisites getter (@0x20b9555c0) returns [JSStack.Prerequisite],
 * which the bootstrap awaits; the first prerequisite's JetEngine.Promise never resolves (measured: not a
 * thread, not an AMS promise, no network). This replaces that array with the EMPTY array singleton
 * (__swiftEmptyArrayStorage @0x1ef0d1df0, the value a Swift [] literal is), so the bootstrap awaits
 * nothing and proceeds straight to loading the AppStore bag + object graph. If the catalog then advances,
 * the prerequisites were the wall; off by default (HAKIOS_EMPTY_PREREQS=1). Regular getter, not async.
 */
static void *(*old_prereqs)(void);

static void *
new_prereqs(void)
{
	static int told;

	if (told < 2) {
		told++;
		say("hakios: store: bootstrap prerequisites replaced with the empty array\n");
	}
	return (void *)(0x1ef0d1df0UL + slide);
}

/*
 * TRACK JETENGINE PROMISES. The bootstrap stalls on an unresolved JetEngine.Promise before the bag; it is
 * not on a thread (async) and not an AMS promise. This hooks JetEngine.Promise's plain init
 * (_$s9JetEngine7PromiseCACyxGycfC @0x1a6e999a8) and records the CALLER of the FIRST promises created
 * once the bootstrap is running (new_askboot_start sets the flag). The prerequisite that creates the stuck
 * promise appears among those creation sites -- the earliest ones, created right after start(). Report
 * prints them (symbolize with scratchpad/names.sh). HAKIOS_TRACK_PROMISES=1, AppStore only.
 */
static struct { void *p; void *caller; bool resolved; } tracked_promises[24];
static int tracked_promise_n;
static bool bootstrap_running;
static volatile int in_recorder;        /* re-entrancy guard: Promise.init() nests */

/*
 * A C passthrough cannot call a Swift constructor through: it does not know the real signature, so it
 * clobbers the generic-metadata / swiftself / swifterror registers and crashes the process (measured:
 * signal 9, the store never bootstraps). The trampoline below leaves EVERY register as the caller set it,
 * calls the original, and only then -- with the return (x0 = the new promise) saved -- calls a C recorder.
 *
 * AND IT RECORDS ONLY THE CALLER'S RETURN ADDRESS (x30 at entry), never a stack walk: these promises are
 * created on async continuations whose "stack" is a chain of heap frames, so frames_from() walking fp
 * faulted (that, not the ABI of the call, is what killed the store on the three-hook version). One
 * address names the creation site -- the prerequisite -- which is all that is needed. Only the plain
 * init() is hooked (0x1a6e999a8): an awaited, externally-resolved prerequisite is a bare Promise, made
 * with init() and fulfilled later; init(task:)/init(value:) are hot async paths and are left alone.
 */
void *old_promise_init_g, *old_promise_resolve_g;

void hakios_record_promise(void *promise, void *caller);        /* called from the trampolines */
void hakios_record_resolved(void *promise);

void
hakios_record_promise(void *promise, void *caller)
{
	if (!bootstrap_running || in_recorder) {
		return;
	}
	in_recorder = 1;
	if (tracked_promise_n < 24 && promise != NULL) {
		tracked_promises[tracked_promise_n].p = promise;
		tracked_promises[tracked_promise_n].caller = caller;
		tracked_promises[tracked_promise_n].resolved = false;
		tracked_promise_n++;
	}
	in_recorder = 0;
}

/*
 * ... and when any promise is resolved (Promise.resolve(_:) @0x1a6e99624), mark the matching tracked one.
 * resolve() is hot and app-wide, but it only ever reads our small table (24 entries), so the cost is a
 * handful of compares. The one tracked promise still unresolved at the report is the stuck prerequisite.
 */
void
hakios_record_resolved(void *promise)
{
	if (promise == NULL) {
		return;
	}
	for (int i = 0; i < tracked_promise_n; i++) {
		if (tracked_promises[i].p == promise) {
			tracked_promises[i].resolved = true;
			return;
		}
	}
}

/* one trampoline: save fp/lr (lr = the caller's creation site); call the original with every caller
 * register intact; save its x0 return; call hakios_record_promise(x0, saved_lr); restore x0; return. */
__asm__(
"	.text\n"
"	.p2align 2\n"
"_hakios_tramp_init:\n"
"	pacibsp\n"
"	stp x29, x30, [sp, #-16]!\n"
"	mov x29, sp\n"
"	adrp x17, _old_promise_init_g@PAGE\n"
"	ldr  x17, [x17, _old_promise_init_g@PAGEOFF]\n"
"	blr  x17\n"
"	stp x0, x1, [sp, #-16]!\n"
"	ldr  x1, [x29, #8]\n"           /* the saved lr = where init() was called from */
"	bl  _hakios_record_promise\n"
"	ldp x0, x1, [sp], #16\n"
"	ldp x29, x30, [sp], #16\n"
"	retab\n"
"	.p2align 2\n"
"_hakios_tramp_resolve:\n"                      /* Promise.resolve(_:): self (the promise) is in x20 */
"	pacibsp\n"
"	stp x29, x30, [sp, #-16]!\n"
"	mov x29, sp\n"
"	str x20, [sp, #-16]!\n"                 /* save self before the call; keep 16-byte alignment */
"	adrp x17, _old_promise_resolve_g@PAGE\n"
"	ldr  x17, [x17, _old_promise_resolve_g@PAGEOFF]\n"
"	blr  x17\n"                             /* original resolve: x20,x0-x7,x8 untouched; returns void */
"	ldr  x0, [sp], #16\n"                   /* x0 = the resolved promise */
"	bl  _hakios_record_resolved\n"
"	ldp x29, x30, [sp], #16\n"
"	retab\n"
/* hakios_call_resolve(promise, arg, fn): call a Swift Promise method whose self is in x20 (swiftself)
 * with the one argument (the error, or a pointer to the value) in x0. Used to force-resolve a stuck
 * bootstrap promise from our own thread. */
"	.p2align 2\n"
"_hakios_call_resolve:\n"                        /* x0=promise, x1=arg, x2=fn */
"	pacibsp\n"
"	stp x29, x30, [sp, #-16]!\n"
"	mov x29, sp\n"
"	str x20, [sp, #-16]!\n"                 /* preserve caller's x20 (callee-saved) */
"	mov x16, x2\n"
"	mov x20, x0\n"                          /* swiftself = the promise */
"	mov x0, x1\n"                           /* x0 = the argument (error word) */
"	blr x16\n"
"	ldr x20, [sp], #16\n"
"	ldp x29, x30, [sp], #16\n"
"	retab\n"
);
extern void hakios_tramp_init(void);
extern void hakios_tramp_resolve(void);
extern void hakios_call_resolve(void *promise, void *arg, void *fn);

static void
dump_tracked_promises(void)
{
	if (tracked_promise_n == 0) {
		return;
	}
	say("hakios: store: %d JetEngine promises created early in the bootstrap:\n", tracked_promise_n);
	for (int i = 0; i < tracked_promise_n; i++) {
		say("hakios: store: promise #%d %p made from %#lx (unslid) %s\n", i, tracked_promises[i].p,
		    (uintptr_t)tracked_promises[i].caller - slide,
		    tracked_promises[i].resolved ? "resolved" : "<-- PENDING (never resolved)");
	}
}

/*
 * FORCE the stuck bootstrap prerequisites to fail (HAKIOS_FORCE_RESOLVE=1). On 16.5 the catalog loaded
 * even though prerequisites failed (AMSErrorDomain code 2); the iOS 27 bootstrap instead hangs on one
 * whose promise never resolves at all. This fails the first two tracked promises (the AppStoreKit roots,
 * #0 from func_20b45d7dc / #1 from func_20b45ec50) with an NSError, to see whether the bootstrap then
 * proceeds (requests its bag / reaches amp-api) the way 16.5 did, or aborts. resolve(toError:) completes
 * a promise regardless of its value type, so no type is guessed. One shot, from the report thread.
 */
/* the empty Array<T> value is the address of libswiftCore's __swiftEmptyArrayStorage singleton
 * (0x1ef0d1df0 unslid, external __AUTH.__data); it is immortal, so a value-witness retain of it is safe,
 * and it is a valid empty array for ANY element type. */
static void *empty_array_value;

static void
force_resolve_prereqs(void)
{
	static bool done;
	const char *on = knob("HAKIOS_FORCE_RESOLVE");
	void *resolve = (void *)(0x1a6e99624UL + slide);        /* Promise.resolve(_:) = the SUCCESS path */
	void *p;

	if (done || on == NULL || *on == '0' || slide == 0 || tracked_promise_n == 0) {
		return;
	}
	done = true;
	/*
	 * VALUE (SUCCESS) PATH, on the main queue. Rejecting (resolve(toError:)) trapped in a hard Swift
	 * assertion inside Promise.reject's continuation, from any thread (runs 223031/224207). resolve(_:)
	 * takes the SUCCESS continuation instead, and a pointer to the value in x0. Promise #0 (func_20b45d7dc)
	 * is over Array<T> (Ghidra: Say...G), so an empty array -- type-agnostic -- is a valid success value.
	 * If the bootstrap then proceeds, the prerequisite was a local gate; if it hangs on #1, try that next.
	 */
	empty_array_value = (void *)(0x1ef0d1df0UL + slide);
	p = tracked_promises[0].p;
	if (p != NULL && !tracked_promises[0].resolved) {
		say("hakios: store: FORCING promise #0 %p to SUCCEED with an empty array (main queue)\n", p);
		dispatch_async(dispatch_get_main_queue(), ^{
			hakios_call_resolve(p, &empty_array_value, resolve);
		});
	}
}

static void (*old_did_complete)(unsigned long);

static void
new_did_complete(unsigned long phase)
{
	say("hakios: store(%s): bootstrap phase completed: %lu\n", who_i_am(), phase);
	if (old_did_complete != NULL) {
		old_did_complete(phase);        /* x20 (self) is callee-saved, untouched here */
	}
}

/*
 * ... and the three prerequisite closures the bootstrap awaits (func_20b45c018 / 20b45de50 / 20b960ce4).
 * start() entered but zero phases completed, so the FIRST prerequisite never finishes. Entry-logging each
 * tells which one is reached; the one reached whose phase never completes is the stall. Call-through keeps
 * the swift context registers (callee-saved) untouched, as with start().
 */
static void (*old_prereq_a)(void);
static void (*old_prereq_b)(void);
static void (*old_prereq_c)(void);

static void
new_prereq_a(void)
{
	static int told;
	if (told < 2) { told++; say("hakios: store: prerequisite A (20b45c018) entered\n"); }
	if (old_prereq_a != NULL) old_prereq_a();
}
static void
new_prereq_b(void)
{
	static int told;
	if (told < 2) { told++; say("hakios: store: prerequisite B (20b45de50) entered\n"); }
	if (old_prereq_b != NULL) old_prereq_b();
}
static void
new_prereq_c(void)
{
	static int told;
	if (told < 2) { told++; say("hakios: store: prerequisite C (20b960ce4) entered\n"); }
	if (old_prereq_c != NULL) old_prereq_c();
}

static void (*old_askboot_start)(void);

static void
new_askboot_start(void)
{
	static int told;

	bootstrap_running = true;
	if (told < 3) {
		told++;
		say("hakios: store(%s): ASKBootstrapV2.start() entered\n", who_i_am());
		install_cont_spy();     /* only now, so the app's own launch is not hooked (that hung it) */
	}
	if (old_askboot_start != NULL) {
		old_askboot_start();
	}
}

/*
 * TYPE SPY (HAKIOS_TYPE_SPY=1). The stuck prerequisite builds an Array<T> and the whole chain is dynamic
 * dispatch, so static tools cannot read T. But every Swift generic type is realised at runtime through
 * swift_getTypeByMangledNameInContext, and swift_getTypeName hands back its demangled name. Hooking the
 * first (a plain C function -- safe) and naming each result while the bootstrap runs prints "Array<...>"
 * and the prerequisite types in clear, which names the device state T stands for.
 */
typedef struct { const char *data; unsigned long length; } hakios_type_name;
static const void *(*old_get_type_by_name)(const char *, unsigned long, const void *, const void *const *);
static hakios_type_name (*swift_type_name_fn)(const void *, int);

static const void *
new_get_type_by_name(const char *name, unsigned long len, const void *ctx, const void *const *args)
{
	const void *md = old_get_type_by_name(name, len, ctx, args);
	static int said;

	if (bootstrap_running && md != NULL && swift_type_name_fn != NULL && said < 500) {
		hakios_type_name tn = swift_type_name_fn(md, 1);

		if (tn.data != NULL && tn.length > 0 && tn.length < 300) {
			said++;
			say("hakios: type #%d: %.*s\n", said, (int)tn.length, tn.data);
		}
	}
	return md;
}

/*
 * ... and, because func_20b45d7dc's Array<T> is realised ONCE and then served from a per-call-site cache
 * (so the hook above, which fires only while the bootstrap runs, misses it), read those caches directly.
 * func_20b45d7dc instantiates its types through the cache words at 0x2763ecbf0 / 0x2763ec610, and
 * func_20b45ec50 through 0x2763ec500; after they have run, each holds the concrete Metadata*. Naming it
 * with swift_getTypeName spells out T -- the thing the stuck prerequisite produces.
 */
static void
dump_prereq_types(void)
{
	static bool done;
	const char *on = knob("HAKIOS_TYPE_SPY");
	unsigned long addrs[] = { 0x2763ecbf0UL, 0x2763ec610UL, 0x2763ec500UL };

	if (done || on == NULL || *on == '0' || slide == 0 || swift_type_name_fn == NULL) {
		return;
	}
	done = true;
	for (unsigned i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
		void **cache = (void **)(addrs[i] + slide);
		void *md = *cache;
		void *clean = (void *)((unsigned long)md & ~3UL);

		if (clean != NULL) {
			hakios_type_name tn = swift_type_name_fn(clean, 1);

			if (tn.data != NULL && tn.length > 0 && tn.length < 400) {
				say("hakios: store: PREREQ TYPE [%#lx] = %.*s\n", addrs[i], (int)tn.length, tn.data);
			} else {
				say("hakios: store: PREREQ TYPE [%#lx] = (no name, md %p)\n", addrs[i], clean);
			}
		} else {
			say("hakios: store: PREREQ TYPE [%#lx] = (cache empty)\n", addrs[i]);
		}
	}
}

/*
 * CONTINUATION SPY (HAKIOS_CONT_SPY=1). The real gate is a SUSPENDED async task. A task suspends by
 * awaiting an UnsafeContinuation (swift_continuation_await) that some callback resumes later
 * (swift_continuation_resume / throwingResume / throwingResumeWithError). The one that is awaited but
 * never resumed IS the stuck await; the caller of swift_continuation_await names the function doing it.
 * These are plain exported C functions (dlsym), hooked with the same register-preserving trampoline as
 * the promise tracker; the continuation arg is in x0 in all of them.
 */
static struct { void *cont; void *caller; bool resumed; } tracked_conts[64];
static int tracked_cont_n;
static volatile int in_cont_rec;
void *old_cont_await_g, *old_cont_resume_g, *old_cont_tres_g, *old_cont_treserr_g;

void hakios_record_cont_await(void *cont, void *caller);
void hakios_record_cont_resumed(void *cont);

void
hakios_record_cont_await(void *cont, void *caller)
{
	if (!bootstrap_running || in_cont_rec || cont == NULL) {
		return;
	}
	in_cont_rec = 1;
	if (tracked_cont_n < 64) {
		tracked_conts[tracked_cont_n].cont = cont;
		tracked_conts[tracked_cont_n].caller = caller;
		tracked_conts[tracked_cont_n].resumed = false;
		tracked_cont_n++;
	}
	in_cont_rec = 0;
}

void
hakios_record_cont_resumed(void *cont)
{
	if (cont == NULL) {
		return;
	}
	for (int i = 0; i < tracked_cont_n; i++) {
		if (tracked_conts[i].cont == cont) {
			tracked_conts[i].resumed = true;
			return;
		}
	}
}

__asm__(
"	.text\n	.p2align 2\n"
"_hakios_tramp_cont_init:\n"                            /* swift_continuation_init(ctx,flags)->task; lr=site */
"	pacibsp\n	stp x29, x30, [sp, #-16]!\n	mov x29, sp\n"
"	adrp x17, _old_cont_await_g@PAGE\n	ldr x17, [x17, _old_cont_await_g@PAGEOFF]\n	blr x17\n"
"	stp x0, x1, [sp, #-16]!\n"                      /* x0 = the returned task (what resume later takes) */
"	ldr x1, [x29, #8]\n	bl _hakios_record_cont_await\n"  /* record(task, caller); x0 already = task */
"	ldp x0, x1, [sp], #16\n	ldp x29, x30, [sp], #16\n	retab\n"
"	.p2align 2\n_hakios_tramp_cont_resume:\n"
"	pacibsp\n	stp x29, x30, [sp, #-16]!\n	mov x29, sp\n	str x0, [sp, #-16]!\n"
"	adrp x17, _old_cont_resume_g@PAGE\n	ldr x17, [x17, _old_cont_resume_g@PAGEOFF]\n	blr x17\n"
"	ldr x0, [sp], #16\n	bl _hakios_record_cont_resumed\n	ldp x29, x30, [sp], #16\n	retab\n"
"	.p2align 2\n_hakios_tramp_cont_tres:\n"
"	pacibsp\n	stp x29, x30, [sp, #-16]!\n	mov x29, sp\n	str x0, [sp, #-16]!\n"
"	adrp x17, _old_cont_tres_g@PAGE\n	ldr x17, [x17, _old_cont_tres_g@PAGEOFF]\n	blr x17\n"
"	ldr x0, [sp], #16\n	bl _hakios_record_cont_resumed\n	ldp x29, x30, [sp], #16\n	retab\n"
"	.p2align 2\n_hakios_tramp_cont_treserr:\n"
"	pacibsp\n	stp x29, x30, [sp, #-16]!\n	mov x29, sp\n	str x0, [sp, #-16]!\n"
"	adrp x17, _old_cont_treserr_g@PAGE\n	ldr x17, [x17, _old_cont_treserr_g@PAGEOFF]\n	blr x17\n"
"	ldr x0, [sp], #16\n	bl _hakios_record_cont_resumed\n	ldp x29, x30, [sp], #16\n	retab\n"
);
extern void hakios_tramp_cont_init(void);
extern void hakios_tramp_cont_resume(void);
extern void hakios_tramp_cont_tres(void);
extern void hakios_tramp_cont_treserr(void);

static void
dump_tracked_conts(void)
{
	int pend = 0;

	for (int i = 0; i < tracked_cont_n; i++) {
		if (!tracked_conts[i].resumed) {
			pend++;
		}
	}
	say("hakios: store: %d continuations awaited in the bootstrap, %d still PENDING\n", tracked_cont_n, pend);
	for (int i = 0; i < tracked_cont_n; i++) {
		if (!tracked_conts[i].resumed) {
			say("hakios: store: CONT PENDING #%d %p awaited from %#lx (unslid)\n", i,
			    tracked_conts[i].cont, (uintptr_t)tracked_conts[i].caller - slide);
		}
	}
}

static void
install_cont_spy(void)
{
	static bool done;
	const char *cspy = knob("HAKIOS_CONT_SPY");
	MSHookFunction_t hook = substrate_hook();
	void *init, *re, *tr, *te;

	if (done || cspy == NULL || *cspy == '0' || hook == NULL) {
		return;
	}
	done = true;
	init = dlsym(RTLD_DEFAULT, "swift_continuation_init");
	re = dlsym(RTLD_DEFAULT, "swift_continuation_resume");
	tr = dlsym(RTLD_DEFAULT, "swift_continuation_throwingResume");
	te = dlsym(RTLD_DEFAULT, "swift_continuation_throwingResumeWithError");
	if (init != NULL) {
		hook(init, (void *)hakios_tramp_cont_init, (void **)&old_cont_await_g);
	}
	if (re != NULL) {
		hook(re, (void *)hakios_tramp_cont_resume, (void **)&old_cont_resume_g);
	}
	if (tr != NULL) {
		hook(tr, (void *)hakios_tramp_cont_tres, (void **)&old_cont_tres_g);
	}
	if (te != NULL) {
		hook(te, (void *)hakios_tramp_cont_treserr, (void **)&old_cont_treserr_g);
	}
	say("hakios: store: continuation hooks installed at start() (init@%p); un-resumed = the stuck await\n", init);
}

static void
hook_bootstrap(void)
{
	static bool done;
	const char *on = knob("HAKIOS_TRACE_BOOTSTRAP");
	const char *track = knob("HAKIOS_TRACK_PROMISES");
	const char *tspy = knob("HAKIOS_TYPE_SPY");
	const char *cspy = knob("HAKIOS_CONT_SPY");
	bool want = (on != NULL && *on != '0') || (track != NULL && *track != '0')
	    || (tspy != NULL && *tspy != '0') || (cspy != NULL && *cspy != '0');
	MSHookFunction_t hook = substrate_hook();
	const char *me = who_i_am();

	if (done || !want || hook == NULL || slide == 0) {
		return;
	}
	if (me == NULL || strcmp(me, "AppStore") != 0) {
		return;
	}
	/* only once AppStoreKit is in the process -- a Swift class of its is the signal */
	if (objc_getClass("ASKRebootstrapCoordinator") == Nil) {
		return;
	}
	done = true;
	/* start() and didComplete are ordinary methods -- safe to hook by address; start() is what sets
	 * bootstrap_running, so the promise tracker records only promises made after the bootstrap begins. */
	hook((void *)(0x20b5d6b80UL + slide), (void *)new_did_complete, (void **)&old_did_complete);
	hook((void *)(0x20b45c080UL + slide), (void *)new_askboot_start, (void **)&old_askboot_start);
	if (track != NULL && *track != '0') {
		hook((void *)(0x1a6e999a8UL + slide), (void *)hakios_tramp_init, &old_promise_init_g);
		hook((void *)(0x1a6e99624UL + slide), (void *)hakios_tramp_resolve, &old_promise_resolve_g);
		say("hakios: store: JetEngine.Promise init+resolve tracked (recording starts at start())\n");
	}
	/* CONT_SPY is installed later, from new_askboot_start (start()), not here: hooking the continuation
	 * runtime at launch hung the app. hook_bootstrap still runs (the gate includes cspy) to put the start()
	 * hook in, which is what then calls install_cont_spy(). */
	if (tspy != NULL && *tspy != '0') {
		void *gt = dlsym(RTLD_DEFAULT, "swift_getTypeByMangledNameInContext");

		swift_type_name_fn = (hakios_type_name (*)(const void *, int))dlsym(RTLD_DEFAULT, "swift_getTypeName");
		if (gt != NULL && swift_type_name_fn != NULL) {
			hook(gt, (void *)new_get_type_by_name, (void **)&old_get_type_by_name);
			say("hakios: store: Swift type instantiation is spied (names printed during bootstrap)\n");
		} else {
			say("hakios: store: TYPE_SPY: getTypeByMangledNameInContext %s, getTypeName %s\n",
			    gt ? "found" : "MISSING", swift_type_name_fn ? "found" : "MISSING");
		}
	}
	if (knob("HAKIOS_EMPTY_PREREQS") != NULL && *knob("HAKIOS_EMPTY_PREREQS") != '0') {
		hook((void *)(0x20b9555c0UL + slide), (void *)new_prereqs, (void **)&old_prereqs);
		say("hakios: store: ASKBootstrapV2.prerequisites will return empty\n");
	}
	/*
	 * These three are by-address hooks on partial-apply THUNKS (func_20b45c018 etc.), and they SIGBUS the
	 * app: the thunk does a Swift value-witness copy (swift_cvw_initWithCopyImpl -> memmove) off a context
	 * register the C hook clobbers. Confirmed dead (run 205505). Off by default, behind their own knob.
	 */
	if (knob("HAKIOS_TRACE_THUNKS") != NULL && *knob("HAKIOS_TRACE_THUNKS") != '0') {
		hook((void *)(0x20b45c018UL + slide), (void *)new_prereq_a, (void **)&old_prereq_a);
		hook((void *)(0x20b45de50UL + slide), (void *)new_prereq_b, (void **)&old_prereq_b);
		hook((void *)(0x20b960ce4UL + slide), (void *)new_prereq_c, (void **)&old_prereq_c);
	}
	say("hakios: store(%s): bootstrap is traced (didComplete @%p, start @%p)\n", me,
	    (void *)(0x20b5d6b80UL + slide), (void *)(0x20b45c080UL + slide));
}

static void
hook_fairplay(void)
{
	const char *off = knob("HAKIOS_NO_FAIRPLAY_HOOK");
	MSHookFunction_t hook = substrate_hook();
	/*
	 * dlsym never finds it: _AMSFairPlayGetDeviceType is in AppleMediaServices' local symbols, not its
	 * exports (the name is in the cache's symbol table, which is why a2s prints it). So it is taken at
	 * its address, 0x18c912aa4 unslid on 24B119, the same way inj26.sh takes SpringBoard's methods --
	 * and only when the shared cache is where this build's is, which is what the slide is measured from.
	 */
	void *target = dlsym(RTLD_DEFAULT, "AMSFairPlayGetDeviceType");

	if (target == NULL && slide != 0 && objc_getClass("AMSUserAgent") != Nil) {
		target = (void *)(0x18c912aa4UL + slide);
	}

	static bool said, done;

	if (done) {
		return;
	}
	if ((off != NULL && *off != '0') || hook == NULL || target == NULL) {
		/*
		 * AppleMediaServices is not in the process when a library dyld inserted is initialized, so
		 * this is tried again from image_added as each image arrives -- before that image's own
		 * initializers run, which is early enough that nothing of AMS has called FairPlay yet.
		 */
		if (!said) {
			said = true;
			say("hakios: store: FairPlay's own function is NOT replaced yet (%s)\n",
			    off != NULL && *off != '0' ? "asked not to" : hook == NULL ? "no substrate"
			    : "AppleMediaServices is not loaded yet; will try again as images arrive");
		}
		if (off != NULL && *off != '0') {
			done = true;
		}
		return;
	}
	done = true;
	hook(target, (void *)new_fp_get_device_type, (void **)&old_fp_get_device_type);
	say("hakios: store: AMSFairPlayGetDeviceType (%p) is replaced before anything can block in it\n",
	    target);
	(void)0;
}

static void
hook_xpc_sessions(void)
{
	const char *wanted = knob("HAKIOS_XPC_SESSIONS");
	void *substrate;
	MSHookFunction_t hook;
	void *create, *activate, *send;

	if (wanted != NULL && *wanted == '0') {
		return;
	}
	substrate = dlopen("/var/jb/usr/lib/libsubstrate.dylib", RTLD_NOW);
	if (substrate == NULL) {
		substrate = dlopen("/var/jb/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate", RTLD_NOW);
	}
	hook = substrate != NULL ? (MSHookFunction_t)dlsym(substrate, "MSHookFunction") : NULL;
	create = dlsym(RTLD_DEFAULT, "xpc_session_create_mach_service");
	activate = dlsym(RTLD_DEFAULT, "xpc_session_activate");
	send = dlsym(RTLD_DEFAULT, "xpc_session_send_message_with_reply_sync");
	rich_desc = (rich_desc_fn)dlsym(RTLD_DEFAULT, "xpc_rich_error_copy_description");
	xpc_desc = (rich_desc_fn)dlsym(RTLD_DEFAULT, "xpc_copy_description");
	{
		void *from_connection = dlsym(RTLD_DEFAULT, "_xpc_session_create_from_connection_4SWIFT");
		void *xpc_service = dlsym(RTLD_DEFAULT, "xpc_session_create_xpc_service");

		connection_name = (const char *(*)(void *))dlsym(RTLD_DEFAULT, "xpc_connection_get_name");
		if (hook != NULL && from_connection != NULL) {
			hook(from_connection, (void *)new_session_from_connection,
			    (void **)&old_session_from_connection);
		}
		void *one_way = dlsym(RTLD_DEFAULT, "xpc_session_send_message");

		if (hook != NULL && xpc_service != NULL) {
			hook(xpc_service, (void *)new_session_create_service, (void **)&old_session_create_service);
		}
		void *with_reply = dlsym(RTLD_DEFAULT, "xpc_session_send_message_with_reply_async");

		if (hook != NULL && one_way != NULL) {
			hook(one_way, (void *)new_session_send, (void **)&old_session_send);
		}
		if (hook != NULL && with_reply != NULL) {
			hook(with_reply, (void *)new_session_send_async, (void **)&old_session_send_async);
		}
	}
	if (hook == NULL || create == NULL || activate == NULL || send == NULL) {
		say("hakios: store: xpc sessions are not named (substrate %s, create %s, activate %s, send %s)\n",
		    hook != NULL ? "yes" : "no", create != NULL ? "yes" : "no", activate != NULL ? "yes" : "no",
		    send != NULL ? "yes" : "no");
		return;
	}
	hook_all_logs(hook);
	hook(create, (void *)new_session_create, (void **)&old_session_create);
	hook(activate, (void *)new_session_activate, (void **)&old_session_activate);
	hook(send, (void *)new_session_send_sync, (void **)&old_session_send_sync);
	say("hakios: store: xpc sessions will be named\n");
}

/*
 * Off unless asked for (exec_env=NAME:HAKIOS_NAME_SERVICES=1). Naming services means saying a line from
 * inside the function that opens a connection, and where a process's os_log goes to logd over XPC rather
 * than to standard error, logging opens connections: the line about a connection opens a connection,
 * which is logged. appstored died of that recursion -- stack overflows that appeared only in the runs
 * whose os_log was not redirected, and that no re-entrancy guard removed entirely. A diagnostic that
 * breaks what it measures is not left on.
 */
static void
hook_mach_services(void)
{
	const char *wanted = knob("HAKIOS_NAME_SERVICES");

	if (wanted == NULL || *wanted == '0') {
		return;
	}
	void *substrate = dlopen("/var/jb/usr/lib/libsubstrate.dylib", RTLD_NOW);
	MSHookFunction_t hook;
	void *target;

	if (substrate == NULL) {
		substrate = dlopen("/var/jb/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate", RTLD_NOW);
	}
	if (substrate == NULL) {
		say("hakios: store: no substrate: mach services are not named\n");
		return;
	}
	hook = (MSHookFunction_t)dlsym(substrate, "MSHookFunction");
	target = dlsym(RTLD_DEFAULT, "xpc_connection_create_mach_service");
	if (hook == NULL || target == NULL) {
		say("hakios: store: no MSHookFunction or no xpc_connection_create_mach_service\n");
		return;
	}
	hook(target, (void *)new_xpc_mach_service, (void **)&old_xpc_mach_service);
	say("hakios: store: mach services will be named (xpc_connection_create_mach_service at %p)\n", target);
}

/*
 * Where the Mescal signature is made on iOS 26/27, and why ours was never asked for. The absinthe step
 * does not sign anything itself: it asks +[AMSDefaults enableRemoteSecuritySigning] and then either
 * -[AMSSigningSecurityService headersForRequest:buyParams:bag:] (another process's service) or
 * +[AMSAbsinthe headersForRequest:buyParams:bag:] (in this process, which is what reaches
 * -[AMSMescalSession signData:bag:error:] -- the method iOS 16.5 used and the one our sapd hook
 * replaces). With remote signing on, the signature is asked of a service this port does not run, so
 * nothing is signed, nothing asks sapd, and the storefront token goes out bare. Answered NO, so the
 * signing stays in this process and reaches sapd.
 */
static BOOL
no_remote_signing(__unused id self, __unused SEL cmd)
{
	static int told;

	if (!told) {
		told = 1;
		say("hakios: store: remote security signing answered NO: the signature is made here, by sapd\n");
	}
	return NO;
}

/*
 * Which service the daemon is talking to. appstored hangs in a synchronous Mach message inside
 * AppleMediaServices' Swift code (thread 3: mach_msg_overwrite), and the message says nothing about
 * whom it is for. Every Mach-service connection names itself here, so the last one made before the
 * silence is the service that never answers.
 */
static id (*old_xpc_init)(id, SEL, id, unsigned long);

static id
new_xpc_init(id self, SEL cmd, id name, unsigned long options)
{
	char text[160];

	say("hakios: store: XPC connection to %s\n", name != NULL ? text_of(name, text, sizeof(text)) : "(no name)");
	return old_xpc_init(self, cmd, name, options);
}

static const char *
who_i_am(void)
{
	const char *name = getprogname();

	return name != NULL ? name : "?";
}

/*
 * AN ACCOUNT WHERE THE PHONE HAS NONE (HAKIOS_FAKE_ACCOUNT=1).
 *
 * -[ACAccountStore ams_localiTunesAccount] answers nil here, and AMS says so sixteen times a run --
 * "Local account not found", through XPC, which is accountsd's answer and not ours. iOS makes that
 * record itself on a device that has been set up; this one has never been, so nothing ever made it.
 * Everything the store does afterwards is about an account: the storefront is saved into one, the bag
 * is fetched for one, and AppStoreKit's own launch (its BootstrapPhase goes initialLoad -> onboarding
 * -> presentingContent) does not leave the first phase. So one is made here, of the iTunes Store type
 * and nothing else: no DSID, no credentials, not saved to the database. Our storefront answers already
 * reply for any account, so an account that exists is an account with a storefront.
 */
static id made_account;         /* the one we made, recognised again by its address */

static id
fabricated_account(id store)
{
	id *const madep = &made_account;
#define made (*madep)
	Class account_class = objc_getClass("ACAccount");
	id type;

	if (made != NULL || account_class == Nil) {
		return made;
	}
	/*
	 * com.apple.account.iTunesStore, which is what accountsd calls it (-_addiTunesStoreAccountType makes
	 * that one and a .sandbox beside it). This file asked for "com.apple.account.iTunes" for weeks, and
	 * every "the iTunes type is MISSING" it printed was that wrong name coming back, not the phone.
	 */
	type = ((id (*)(id, SEL, id))objc_msgSend)(store, sel_registerName("accountTypeWithAccountTypeIdentifier:"),
	    (id)CFSTR("com.apple.account.iTunesStore"));
	{
		/* What the database actually gives this process, which is how to tell a row accountsd cannot
		 * read from a row it reads and does not like. */
		id all = ((id (*)(id, SEL))objc_msgSend)(store, sel_registerName("accounts"));
		long n = all != NULL ? ((long (*)(id, SEL))objc_msgSend)(all, sel_registerName("count")) : -1;
		char text[200] = "?";

		say("hakios: store: this store has %ld account(s); the iTunes type is %s; they are %.150s\n",
		    n, type != NULL ? "there" : "MISSING", all != NULL ? text_of(all, text, sizeof(text)) : "(nil)");
	}
	if (type == NULL) {
		/*
		 * ... and if the database has not even got the TYPE -- which is how a phone that was never
		 * set up starts, ZACCOUNTTYPE empty -- one is made here too. ACAccountType is a Core Data
		 * object, so its identifier is set through the key rather than a setter that may not be
		 * declared. Nothing is saved: this exists for the length of the process, which is all AMS
		 * needs to have an account to ask about.
		 */
		Class type_class = objc_getClass("ACAccountType");

		type = type_class != Nil ? ((id (*)(id, SEL))objc_msgSend)((id)type_class,
		    sel_registerName("alloc")) : NULL;
		type = type != NULL ? ((id (*)(id, SEL))objc_msgSend)(type, sel_registerName("init")) : NULL;
		if (type != NULL) {
			((void (*)(id, SEL, id, id))objc_msgSend)(type, sel_registerName("setValue:forKey:"),
			    (id)CFSTR("com.apple.account.iTunesStore"), (id)CFSTR("identifier"));
			((void (*)(id, SEL, id, id))objc_msgSend)(type, sel_registerName("setValue:forKey:"),
			    (id)CFSTR("iTunes Store"), (id)CFSTR("accountTypeDescription"));
		}
		say("hakios: store: the database has no iTunes account type; one was %s here\n",
		    type != NULL ? "made" : "NOT made");
		if (type == NULL) {
			return NULL;
		}
	}
	made = ((id (*)(id, SEL))objc_msgSend)((id)account_class, sel_registerName("alloc"));
	made = made != NULL ? ((id (*)(id, SEL, id))objc_msgSend)(made, sel_registerName("initWithAccountType:"),
	    type) : NULL;
	/*
	 * ... and offered to accountsd, which is the supported way to make one exist: a row written into
	 * Accounts3.sqlite by hand is not seen (the TYPE written that way is, both here and in appstored,
	 * so the file is read -- the account row is refused). -[ACAccountStore ams_saveAccount:] is AMS's
	 * own synchronous save; if the daemon takes it, the account is in the database from then on and
	 * nothing of ours is needed to find it.
	 */
	if (knob("HAKIOS_SAVE_ACCOUNT") != NULL) {
		id saved = ((id (*)(id, SEL, id))objc_msgSend)(store, sel_registerName("ams_saveAccount:"), made);
		char text[160] = "?";

		say("hakios: store: offered the account to accountsd: %s\n",
		    saved != NULL ? text_of(saved, text, sizeof(text)) : "nothing back");
	}
	say("hakios: store: INJECTED a local iTunes account (%p), which this phone has none of\n", (void *)made);
	{
		/*
		 * What AMS means by "local", read off the two constants -[ACAccount ams_isLocalAccount]
		 * compares against (0x18c82ee18 on 24B119): an iTunes account that either has a boolean
		 * account property set, or whose identifier is one particular string. Both live in __DATA
		 * and are only readable once the fixups are done, which is here and not on this machine.
		 * They say what a row in Accounts3.sqlite would have to hold for accountsd itself to answer
		 * "the local account" -- which is the answer the store asks for over XPC and still does not get.
		 */
		id key = *(id *)(uintptr_t)(0x1e0951b58UL + slide);
		id ident = *(id *)(uintptr_t)(0x1e2eb6610UL + slide);
		char one[80] = "?", two[80] = "?";

		say("hakios: store: a local account is one with the property \"%s\" or the identifier \"%s\"\n",
		    text_of(key, one, sizeof(one)), text_of(ident, two, sizeof(two)));
	}
	return made;
#undef made
}

/*
 * ... and it has to say it is the local one. With an account where there was none, AMS gets one step
 * further and stops at "Attempted adding cookies to non-local account": the account it was handed does
 * not answer -ams_isLocalAccount, which on a real phone is a property of the record in the database.
 * Only ours is answered; every other account is left to AMS.
 */
/*
 * THE ACCOUNT THAT CAN ALSO BE A PROMISE.
 *
 * -[ACAccountStore ams_localiTunesAccount] is sent two different messages by two different callers here:
 * -continueWithBlock:, which is a promise's, and -ams_storefrontForMediaType:, which is an account's.
 * Answering with one shape kills the caller that wanted the other -- measured both ways round, and both
 * inside amsaccountsd alone, so it is not a question of which process. So the account answers the
 * promise's message as well: the block is called at once with the account and no error (through the
 * block's own invoke pointer, so this file needs no block support), and what comes back is a finished
 * promise, which is what a caller that chains expects to hold.
 */
struct block_layout {
	void *isa;
	int flags;
	int reserved;
	void (*invoke)(void *, ...);
};

static id
account_continue_with_block(id self, SEL cmd, void *block)
{
	struct block_layout *b = block;
	Class promise_class = objc_getClass("AMSPromise");
	static bool said;

	(void)cmd;
	if (!said) {
		said = true;
		say("hakios: store: the account was sent a promise's message; the block is called with the "
		    "account itself\n");
	}
	if (b != NULL && b->invoke != NULL) {
		b->invoke(b, self, NULL);
	}
	return promise_class != Nil ? ((id (*)(id, SEL, id))objc_msgSend)((id)promise_class,
	    sel_registerName("promiseWithResult:"), NULL) : NULL;
}

static void
teach_account_to_be_a_promise(void)
{
	static bool done;
	Class account_class = objc_getClass("ACAccount");

	if (done || account_class == Nil) {
		return;
	}
	done = true;
	/* only if it has none of its own: an answer of ours must never replace an answer of theirs */
	if (class_getInstanceMethod(account_class, sel_registerName("continueWithBlock:")) == NULL) {
		class_addMethod(account_class, sel_registerName("continueWithBlock:"),
		    (IMP)account_continue_with_block, "@@:@?");
	}
	if (class_getInstanceMethod(account_class, sel_registerName("thenWithBlock:")) == NULL) {
		class_addMethod(account_class, sel_registerName("thenWithBlock:"),
		    (IMP)account_continue_with_block, "@@:@?");
	}
	say("hakios: store: ACAccount now answers -continueWithBlock: and -thenWithBlock:\n");
}

/*
 * THE DOOR ITSELF. -[ACAccountStore ams_localiTunesAccount] is what every one of AMS's account paths
 * calls, and on this phone it answers nil: the accounts database has no account and no account type,
 * because iOS fills that table when a device is set up and this one never has been. Two hooks of ours
 * answered their own callers with a made account; this answers the door, so that everything behind it --
 * the cookies, the storefront save, AppStoreKit's launch -- gets the same account without being named one
 * at a time. The real one is preferred whenever there is one.
 */
/*
 * WHICH TYPE THE APPLICATION ASKS FOR. The daemon now holds one account, of AMS's own type
 * com.apple.account.iTunesStore, and the App Store's own store still answers nothing -- with
 * com.apple.accounts Code=7, "not found", where it used to be Code=9, "not authorized". A name that does
 * not match would read exactly like that, and this library has itself been asking for
 * com.apple.account.iTunes all along, which is a different string. So every identifier anybody asks this
 * class for is said once.
 */
static id (*old_type_with_identifier)(id, SEL, id);

static id
new_type_with_identifier(id self, SEL cmd, id identifier)
{
	id type = old_type_with_identifier != NULL ? old_type_with_identifier(self, cmd, identifier) : NULL;
	char text[90] = "(nil)";
	static char said[8][90];
	static unsigned count;

	text_of(identifier, text, sizeof(text));
	for (unsigned i = 0; i < count; i++) {
		if (strncmp(said[i], text, sizeof(said[0]) - 1) == 0) {
			return type;
		}
	}
	if (count < 8) {
		snprintf(said[count], sizeof(said[0]), "%s", text);
		count++;
	}
	say("hakios: store: the account type \"%.70s\" was asked for: %s\n", text,
	    type != NULL ? "there" : "NOT THERE");
	return type;
}

static id (*old_local_itunes_account)(id, SEL);

static id
new_local_itunes_account(id self, SEL cmd)
{
	id account = old_local_itunes_account != NULL ? old_local_itunes_account(self, cmd) : NULL;
	const char *fake;

	if (account != NULL) {
		return account;
	}
	fake = knob("HAKIOS_FAKE_ACCOUNT");
	if (fake == NULL || *fake == '0') {
		return NULL;           /* nothing of ours by default: the account is a switch, not a policy */
	}
	account = fabricated_account(self);
	/*
	 * ... and only then is ACAccount taught a promise's messages. Adding them unconditionally would
	 * change what -respondsToSelector: says about every account in the process, which is a thing AMS
	 * is entitled to ask and nothing of ours should answer differently unless the account is ours.
	 */
	teach_account_to_be_a_promise();
	/*
	 * ... as a finished promise where the caller wants one (HAKIOS_LOCAL_ACCOUNT_IS_PROMISE=1). In
	 * amsaccountsd the answer is sent -continueWithBlock:, which an ACAccount does not know: the daemon
	 * died of that selector the first time it was given an account (UNCAUGHT NSInvalidArgumentException,
	 * run 020257). It is the same shape as its sibling, _ams_localiTunesAccountForAccountMediaType:,
	 * which answers a promise on iOS 26/27 where 16.5 answered the account.
	 */
	if (account != NULL) {
		/*
		 * In amsaccountsd only. The two processes want different things of the same selector: the
		 * daemon sends the answer -continueWithBlock: (and died of it with an account), and the App
		 * Store sends it -ams_storefrontForMediaType: (and died of it with a promise). Measured both
		 * ways round, one run each.
		 */
		const char *as_promise = knob("HAKIOS_LOCAL_ACCOUNT_IS_PROMISE");
		Class promise_class = as_promise != NULL && *as_promise != '0'
		    ? objc_getClass("AMSPromise") : Nil;

		if (promise_class != Nil) {
			id promise = ((id (*)(id, SEL, id))objc_msgSend)((id)promise_class,
			    sel_registerName("promiseWithResult:"), account);
			static bool said;

			if (!said) {
				said = true;
				say("hakios: store: ams_localiTunesAccount answers a finished promise, not the "
				    "account itself\n");
			}
			return promise;
		}
	}
	return account;
}

static BOOL (*old_is_local_account)(id, SEL);

static BOOL
new_is_local_account(id self, SEL cmd)
{
	if (self != NULL && self == made_account) {
		static bool said;

		if (!said) {
			said = true;
			say("hakios: store: the account we made says it is the local one\n");
		}
		return YES;
	}
	return old_is_local_account != NULL ? old_is_local_account(self, cmd) : NO;
}

/*
 * THE STOREFRONT PUT ON THE ACCOUNT ITSELF (HAKIOS_SET_STOREFRONT=1).
 *
 * With the account types made, AMS makes its own local account -- and then says "The local account
 * doesn't have a storefront", because a storefront is a property of the record and this one has none.
 * Answering -ams_storefront for it is what this library has done all along, and it is not enough: the bag
 * provider looks at the account, not at us. So the value is written where AMS looks for it, with AMS's
 * own setter, and the account is saved. After that nothing of ours has to answer anything: the phone has
 * an iTunes account with a storefront, which is what a phone that has been set up has.
 */
static void
put_storefront_on(id store, id account)
{
	const char *wanted = knob("HAKIOS_SET_STOREFRONT");
	static bool done;
	id saved;

	if (done || account == NULL || wanted == NULL || *wanted == '0') {
		return;
	}
	done = true;
	((void (*)(id, SEL, id))objc_msgSend)(account, sel_registerName("ams_setStorefront:"),
	    (id)storefront);
	saved = ((id (*)(id, SEL, id))objc_msgSend)(store, sel_registerName("ams_saveAccount:"), account);
	{
		char text[160] = "nothing back";

		say("hakios: store: the storefront was put on the account itself; the save said %.120s\n",
		    saved != NULL ? text_of(saved, text, sizeof(text)) : text);
	}
}

static id
local_account_of(id store, const char *who)
{
	/* the original, not our own answer: this wants the account, whatever the door now hands out */
	id account = old_local_itunes_account != NULL ? old_local_itunes_account(store,
	    sel_registerName("ams_localiTunesAccount"))
	    : ((id (*)(id, SEL))objc_msgSend)(store, sel_registerName("ams_localiTunesAccount"));
	static int told;

	{
		/* What the database gives this process, said once whatever else happens. */
		static bool counted;

		if (!counted) {
			id all = ((id (*)(id, SEL))objc_msgSend)(store, sel_registerName("accounts"));
			id type = ((id (*)(id, SEL, id))objc_msgSend)(store,
			    sel_registerName("accountTypeWithAccountTypeIdentifier:"),
			    (id)CFSTR("com.apple.account.iTunesStore"));
			long n = all != NULL ? ((long (*)(id, SEL))objc_msgSend)(all, sel_registerName("count")) : -1;
			char text[200] = "?";

			counted = true;
			id of_type = type != NULL ? ((id (*)(id, SEL, id))objc_msgSend)(store,
			    sel_registerName("accountsWithAccountType:"), type) : NULL;
			long m = of_type != NULL ? ((long (*)(id, SEL))objc_msgSend)(of_type,
			    sel_registerName("count")) : -1;
			char two[260] = "?";

			say("hakios: store: the accounts database gives this process %ld account(s), the iTunes "
			    "type %s and %ld of that type: %.200s\n", n, type != NULL ? "is there" : "is MISSING",
			    m, of_type != NULL ? text_of(of_type, two, sizeof(two)) : "(nil)");
			(void)text;
		}
	}
	if (account == NULL) {
		const char *fake = knob("HAKIOS_FAKE_ACCOUNT");

		if (fake != NULL && *fake != '0') {
			account = fabricated_account(store);
		}
	} else {
		put_storefront_on(store, account);
	}

	/*
	 * ... and not only the first time. appstored's own log says "[ActiveAccountObserver] Initialized
	 * with account: (null)" and then asks "[Bag] Fetching current account" for ever, while this door
	 * answered it an account on the first call -- so either it asks again and gets something else, or
	 * it never comes back here at all. One line cannot tell those apart; five can.
	 */
	if (told < 5) {
		told++;
		char text[260];

		say("hakios: store(%s): %s answered at once (call %d), %s\n", who_i_am(), who, told,
		    account != NULL ? text_of(account, text, sizeof(text)) : "nothing");
	}
	return account;
}

static id
new_active_account(id self, SEL cmd)
{
	(void)cmd;
	return local_account_of(self, "ams_activeiTunesAccount");
}

static id
new_active_account_type(id self, SEL cmd, void *media_type)
{
	(void)cmd, (void)media_type;
	return local_account_of(self, "ams_activeiTunesAccountForMediaType:");
}

/*
 * THE ANSWER THAT NEVER COMES. appstored's own log says what it is doing while the store shows
 * "Loading...": "[Bag] Fetching current account" over and over, "[Migrate] Attempting to load bag",
 * "[Migrate] [Daemon] Migrations needed", and -- the one that matters -- "[General]
 * [ActiveAccountObserver] Initialized with account: (null)". Meanwhile every one of the store's 127
 * requests to com.apple.appstored.xpc.request is cancelled, because the daemon on the other side is
 * still waiting for that account. This library's answers are on ACAccountStore's SYNCHRONOUS doors
 * (ams_activeiTunesAccount and its kin, which are entered and do answer); a daemon asks through the
 * asynchronous ones, and those end in accountsd, whose reply the comment at the bottom of this file
 * already records as never coming ("appstored's account query to it times out after two seconds and
 * the connection it makes next never returns").
 *
 * So these four are the asynchronous doors. Unasked, they only count themselves and pass through --
 * which already says WHICH one a daemon uses. With HAKIOS_ANSWER_ACCOUNT_QUERIES=1 the completion is
 * called here, with the local account, and accountsd is not waited for at all.
 */
static void (*old_accounts_ids_props)(id, SEL, void *, void *, void *);
static void (*old_cached_accounts_ids)(id, SEL, void *, void *);
static void (*old_accounts_type_options)(id, SEL, void *, void *, void *);
static void (*old_visible_accounts_ids)(id, SEL, void *, void *);

/* a block is called the way the runtime calls it: through the invoke in its own layout */
static void
hand_the_accounts_to(void *completion, id store, const char *who)
{
	struct block_layout *b = completion;
	id account = ((id (*)(id, SEL))objc_msgSend)(store, sel_registerName("ams_localiTunesAccount"));
	id accounts = NULL;
	static int told;

	if (account != NULL) {
		accounts = ((id (*)(Class, SEL, id))objc_msgSend)(objc_getClass("NSArray"),
		    sel_registerName("arrayWithObject:"), account);
	} else {
		accounts = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSArray"),
		    sel_registerName("array"));
	}
	if (told < 4) {
		told++;
		say("hakios: store(%s): %s answered here, with %s, instead of waiting for accountsd\n",
		    who_i_am(), who, account != NULL ? "the local account" : "an empty list");
	}
	if (b != NULL && b->invoke != NULL) {
		b->invoke(b, accounts, NULL);
	}
}

static bool
answer_account_queries(void)
{
	const char *on = knob("HAKIOS_ANSWER_ACCOUNT_QUERIES");

	return on != NULL && *on != '0';
}

static void
new_accounts_ids_props(id self, SEL cmd, void *ids, void *props, void *completion)
{
	if (answer_account_queries()) {
		hand_the_accounts_to(completion, self, "accountsWithAccountTypeIdentifiers:preloadedProperties:");
		return;
	}
	if (old_accounts_ids_props != NULL) {
		old_accounts_ids_props(self, cmd, ids, props, completion);
	}
}

static void
new_cached_accounts_ids(id self, SEL cmd, void *ids, void *completion)
{
	if (answer_account_queries()) {
		hand_the_accounts_to(completion, self, "_cachedAccountsWithAccountTypeIdentifiers:");
		return;
	}
	if (old_cached_accounts_ids != NULL) {
		old_cached_accounts_ids(self, cmd, ids, completion);
	}
}

static void
new_accounts_type_options(id self, SEL cmd, void *type, void *options, void *completion)
{
	if (answer_account_queries()) {
		hand_the_accounts_to(completion, self, "accountsWithAccountType:options:");
		return;
	}
	if (old_accounts_type_options != NULL) {
		old_accounts_type_options(self, cmd, type, options, completion);
	}
}

static void
new_visible_accounts_ids(id self, SEL cmd, void *ids, void *completion)
{
	if (answer_account_queries()) {
		hand_the_accounts_to(completion, self, "visibleTopLevelAccountsWithAccountTypeIdentifiers:");
		return;
	}
	if (old_visible_accounts_ids != NULL) {
		old_visible_accounts_ids(self, cmd, ids, completion);
	}
}

/* the synchronous lists, so that a daemon asking through them is not invisible */
static id (*old_accounts_with_type)(id, SEL, void *);
static id (*old_accounts_with_type_id)(id, SEL, void *);

/*
 * A list's description begins with a newline, so printing it says nothing: the count is the measurement.
 * And with HAKIOS_LIST_ACCOUNT=1 an empty list is replaced by the one account this phone does have --
 * appstored asks ams_activeiTunesAccount exactly once (it answers, with the account) and then asks this
 * door, whose answer is what its ActiveAccountObserver keeps as "(null)".
 */
static id
one_account_list(id store)
{
	id account = ((id (*)(id, SEL))objc_msgSend)(store, sel_registerName("ams_localiTunesAccount"));

	if (account == NULL) {
		return NULL;
	}
	return ((id (*)(Class, SEL, id))objc_msgSend)(objc_getClass("NSArray"),
	    sel_registerName("arrayWithObject:"), account);
}

static long
how_many(id list)
{
	return list != NULL ? ((long (*)(id, SEL))objc_msgSend)(list, sel_registerName("count")) : -1;
}

static id
new_accounts_with_type(id self, SEL cmd, void *type)
{
	id answer = old_accounts_with_type != NULL ? old_accounts_with_type(self, cmd, type) : NULL;
	const char *fill = knob("HAKIOS_LIST_ACCOUNT");
	long n = how_many(answer);
	static int told;

	if (fill != NULL && *fill != '0' && n <= 0) {
		id ours = one_account_list(self);

		if (ours != NULL) {
			if (told < 3) {
				told++;
				say("hakios: store(%s): accountsWithAccountType: answered %ld; the local account "
				    "is handed back instead\n", who_i_am(), n);
			}
			return ours;
		}
	}
	if (told < 3) {
		told++;
		say("hakios: store(%s): accountsWithAccountType: answered %ld account(s)\n", who_i_am(), n);
	}
	return answer;
}

static id
new_accounts_with_type_id(id self, SEL cmd, void *identifier)
{
	id answer = old_accounts_with_type_id != NULL ? old_accounts_with_type_id(self, cmd, identifier)
	    : NULL;
	static int told;

	const char *fill = knob("HAKIOS_LIST_ACCOUNT");
	long n = how_many(answer);

	if (fill != NULL && *fill != '0' && n <= 0) {
		id ours = one_account_list(self);

		if (ours != NULL) {
			if (told < 3) {
				told++;
				say("hakios: store(%s): accountsWithAccountTypeIdentifier: answered %ld; the "
				    "local account is handed back instead\n", who_i_am(), n);
			}
			return ours;
		}
	}
	if (told < 3) {
		told++;
		say("hakios: store(%s): accountsWithAccountTypeIdentifier: answered %ld account(s)\n",
		    who_i_am(), n);
	}
	return answer;
}

/*
 * THE CALL THAT NEVER RETURNS. The App Store's bag fetch is entered once and never comes back
 * (-[AMSBagNetworkTask _performFetchWithAttemptedCount:account:storefront:] : entered 1, returned 0),
 * and the thread report says where it stands: mach_msg, under -[NSXPCConnection _sendInvocation:...],
 * under ___55-[AMSDaemonConnection cookieServiceProxySyncWithError:]_block_invoke. It is a SYNCHRONOUS
 * request to the AMS service broker for a cookie-service proxy, and the broker never answers it. Nothing
 * after it is ever reached -- _queryItemsForClientInfo:, the anisette headers, the cookies themselves are
 * all "NEVER ENTERED" -- so this one call is what the whole catalog waits on.
 *
 * With HAKIOS_NO_COOKIE_SERVICE=1 it is answered here, at once, with nothing: a proxy that could not be
 * had is a case the caller must already handle, and this library answers the cookie question itself
 * anyway (ams_addCookiesForAccount:... answers empty).
 */
static id (*old_cookie_props)(id, SEL, void *, BOOL, void *);

/*
 * The local AMSCookieService still reaches the broker: its getCookiePropertiesForAccount:cookieDatabaseOnly:
 * error: opens an AMSDaemonConnection and waits (measured: the hang moved into _initializeConnection). So
 * the synchronous getter itself is answered -- an empty cookie set, no daemon, no wait. Cookies are empty,
 * which is what 16.5 fetched the catalog with.
 */
static id
new_cookie_props(id self, SEL cmd, void *account, BOOL db_only, void *error)
{
	const char *local = knob("HAKIOS_LOCAL_COOKIE_SERVICE");
	static int told;

	(void)self, (void)cmd, (void)account, (void)db_only;
	if (local != NULL && *local != '0') {
		if (error != NULL) {
			*(id *)error = NULL;
		}
		if (told < 3) {
			told++;
			say("hakios: store(%s): cookie properties answered empty, without the daemon\n",
			    who_i_am());
		}
		return ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSArray"), sel_registerName("array"));
	}
	return old_cookie_props != NULL ? old_cookie_props(self, cmd, account, db_only, error) : NULL;
}

static id (*old_cookie_proxy_sync)(id, SEL, void *);
static id (*old_account_proxy_sync)(id, SEL, void *);
static id (*old_security_proxy_sync)(id, SEL, void *);
static id (*old_itfe_proxy_sync)(id, SEL, void *);
static id (*old_abc_proxy_sync)(id, SEL, void *);

/*
 * The broker has five of these, and any of them can stop a process the same way. appstored's own
 * deadlock is of that shape: its cache accessor takes a global os_unfair_lock and HOLDS IT through the
 * slow path (measured in its own code: lock at +0xbce8, unlock only on the cache hit at +0xbd30, while
 * the miss goes on to build the AMS client for "com.apple.appstored"), so one job that never comes back
 * from AMS leaves every other job of the daemon waiting on that lock for ever.
 */
static id
no_broker_service(id self, SEL cmd, void *error, const char *which, id (*original)(id, SEL, void *))
{
	const char *off = knob("HAKIOS_NO_COOKIE_SERVICE");
	static int told;

	if (off != NULL && *off != '0') {
		if (told < 6) {
			told++;
			say("hakios: store(%s): the %s service proxy is answered here, with nothing, "
			    "instead of waiting on the broker\n", who_i_am(), which);
		}
		return NULL;
	}
	return original != NULL ? original(self, cmd, error) : NULL;
}

static id
new_cookie_proxy_sync(id self, SEL cmd, void *error)
{
	const char *local = knob("HAKIOS_LOCAL_COOKIE_SERVICE");
	static int told;

	/*
	 * Returning nil (HAKIOS_NO_COOKIE_SERVICE) unblocks the hang but breaks cookies, and letting the
	 * call through hangs on the broker. The third way: hand back a LOCAL AMSCookieService -- a real
	 * object that answers getCookiePropertiesForAccount:... from the on-device cookie store (empty here)
	 * without any XPC, so cookies "work" empty and nothing waits on amsaccountsd. 16.5 loaded the
	 * catalog with no real cookies at all (Mescal signing), so empty is the right shape.
	 */
	if (local != NULL && *local != '0') {
		Class service = objc_getClass("AMSCookieService");
		id made = service != Nil ? ((id (*)(Class, SEL))objc_msgSend)(service,
		    sel_registerName("alloc")) : NULL;

		made = made != NULL ? ((id (*)(id, SEL))objc_msgSend)(made, sel_registerName("init")) : NULL;
		if (made != NULL) {
			if (told < 3) {
				told++;
				say("hakios: store(%s): the cookie proxy is a local AMSCookieService; no XPC, no "
				    "wait\n", who_i_am());
			}
			return made;
		}
	}
	return no_broker_service(self, cmd, error, "cookie", old_cookie_proxy_sync);
}

static id
new_account_proxy_sync(id self, SEL cmd, void *error)
{
	return no_broker_service(self, cmd, error, "account management", old_account_proxy_sync);
}

static id
new_security_proxy_sync(id self, SEL cmd, void *error)
{
	return no_broker_service(self, cmd, error, "security", old_security_proxy_sync);
}

static id
new_itfe_proxy_sync(id self, SEL cmd, void *error)
{
	return no_broker_service(self, cmd, error, "itfe", old_itfe_proxy_sync);
}

static id
new_abc_proxy_sync(id self, SEL cmd, void *error)
{
	return no_broker_service(self, cmd, error, "auto bug capture", old_abc_proxy_sync);
}

/*
 * Which of the five calls inside the bag fetch does not come back. Statically, -[AMSBagNetworkTask
 * _performFetchWithAttemptedCount:account:storefront:] does: clientInfo, sharedBagConfig, profile,
 * profileVersion, _URLCookieNamesForProfile:, then _queryItemsForClientInfo:... and
 * _createRequestWithQueryItems:. In amsaccountsd the last two are never entered while the cookies are
 * answered, so the stall is in one of the first five. These two say so in one run.
 */
static id (*old_client_info)(id, SEL);
static id (*old_cookie_names)(id, SEL, void *);

static id
new_client_info(id self, SEL cmd)
{
	static int in, out;
	id answer;

	if (in < 3) {
		in++;
		say("hakios: store(%s): -> clientInfo (call %d)\n", who_i_am(), in);
	}
	answer = old_client_info != NULL ? old_client_info(self, cmd) : NULL;
	if (out < 3) {
		out++;
		say("hakios: store(%s): <- clientInfo (call %d)\n", who_i_am(), out);
	}
	return answer;
}

static id
new_cookie_names(id self, SEL cmd, void *profile)
{
	static int in, out;
	id answer;

	if (in < 3) {
		in++;
		say("hakios: store(%s): -> _URLCookieNamesForProfile: (call %d)\n", who_i_am(), in);
	}
	answer = old_cookie_names != NULL ? old_cookie_names(self, cmd, profile) : NULL;
	if (out < 3) {
		out++;
		say("hakios: store(%s): <- _URLCookieNamesForProfile: (call %d)\n", who_i_am(), out);
	}
	return answer;
}

/*
 * The refusal is the CLIENT's, not the daemon's: appstored says "Attempted to register account monitor
 * for types client is not authorized to access" in its own log, and all four of the daemon's gates are
 * replaced and never called. What the client looks at is the account type's access keys
 * (-[ACAccountType accessKeys], -[ACAccountStore accessKeysForAccountType:]). An empty set is a client
 * that may not look. With HAKIOS_GRANT_ACCOUNT_ACCESS=1 the type answers with a set holding its own
 * identifier, which is the shape of a key.
 */
static id (*old_access_keys)(id, SEL);
static id (*old_access_keys_for)(id, SEL, void *);

static id
keys_or_ours(id answer, id type)
{
	long n = answer != NULL ? ((long (*)(id, SEL))objc_msgSend)(answer, sel_registerName("count")) : 0;
	id identifier;
	static int told;

	if (n > 0) {
		return answer;
	}
	identifier = type != NULL ? ((id (*)(id, SEL))objc_msgSend)(type,
	    sel_registerName("identifier")) : NULL;
	if (identifier == NULL) {
		return answer;
	}
	if (told < 3) {
		told++;
		say("hakios: store: the account type had no access keys; its own identifier is answered\n");
	}
	return ((id (*)(Class, SEL, id))objc_msgSend)(objc_getClass("NSSet"),
	    sel_registerName("setWithObject:"), identifier);
}

static id
new_access_keys(id self, SEL cmd)
{
	id answer = old_access_keys != NULL ? old_access_keys(self, cmd) : NULL;
	const char *on = knob("HAKIOS_GRANT_ACCOUNT_ACCESS");

	if (on == NULL || *on == '0') {
		return answer;
	}
	return keys_or_ours(answer, self);
}

static id
new_access_keys_for(id self, SEL cmd, void *type)
{
	id answer = old_access_keys_for != NULL ? old_access_keys_for(self, cmd, type) : NULL;
	const char *on = knob("HAKIOS_GRANT_ACCOUNT_ACCESS");

	if (on == NULL || *on == '0') {
		return answer;
	}
	return keys_or_ours(answer, (id)type);
}

/*
 * THE PLACE THE REFUSAL IS MADE. With every error in accountsd named by its maker, the code-9 the client
 * reports comes from here, every time:
 *   _AMSCustomSubclassedError <- _AMSCustomError
 *   <- -[AMSBagActiveAccountProvider _handleFetchedLocalAccount:fetchError:mustContainStorefront:
 *      mediaType:error:]
 *   <- ___100-[AMSBagActiveAccountProvider _localAccountInStore:mediaType:mustContainStorefront:sync:
 *      completion:]_block_invoke
 * So AMS asks the account store for the local account, is refused, and wraps the refusal. This hands it
 * the account instead: when it was given none and an error, the error is dropped and the account this
 * phone does have is put in its place. HAKIOS_FEED_LOCAL_ACCOUNT=1.
 */
static id (*old_handle_fetched)(id, SEL, void *, void *, BOOL, void *, void *);

static id
new_handle_fetched(id self, SEL cmd, void *account, void *fetch_error, BOOL must_have_storefront,
    void *media_type, void *error)
{
	const char *on = knob("HAKIOS_FEED_LOCAL_ACCOUNT");
	static int told;

	if (on != NULL && *on != '0' && account == NULL) {
		Class store_class = objc_getClass("ACAccountStore");
		id store = store_class != Nil ? ((id (*)(Class, SEL))objc_msgSend)(store_class,
		    sel_registerName("new")) : NULL;
		id ours = store != NULL ? ((id (*)(id, SEL))objc_msgSend)(store,
		    sel_registerName("ams_localiTunesAccount")) : NULL;

		if (ours != NULL) {
			if (told < 3) {
				char text[200];

				told++;
				say("hakios: store(%s): the local account was refused; %s is handed over "
				    "instead\n", who_i_am(), text_of(ours, text, sizeof(text)));
			}
			return old_handle_fetched != NULL ? old_handle_fetched(self, cmd, ours, NULL,
			    must_have_storefront, media_type, error) : ours;
		}
		/*
		 * ... and inside accountsd there is none to be had by asking: ACAccountStore there asks the
		 * daemon, which is this process, which is what refused. So one is made instead -- the same
		 * fabricated account this library hands to the App Store when the database is empty.
		 */
		ours = store != NULL ? fabricated_account(store) : NULL;
		if (ours != NULL) {
			if (told < 3) {
				char text[200];

				told++;
				say("hakios: store(%s): the local account was refused and none could be "
				    "fetched here; a made one is handed over: %s\n", who_i_am(),
				    text_of(ours, text, sizeof(text)));
			}
			return old_handle_fetched != NULL ? old_handle_fetched(self, cmd, ours, NULL,
			    must_have_storefront, media_type, error) : ours;
		}
		if (told < 3) {
			told++;
			say("hakios: store(%s): the local account was refused and this process has none to "
			    "hand over\n", who_i_am());
		}
	}
	return old_handle_fetched != NULL ? old_handle_fetched(self, cmd, account, fetch_error,
	    must_have_storefront, media_type, error) : NULL;
}

/*
 * THE INSTRUCTION THAT REFUSES. Static read of -[ACDAccountStoreFilter accountsWithAccountType:options:
 * completion:]: it calls _isClientPermittedToAccessAccountTypeWithIdentifier: and, when that is NO
 * (cbz w25, ...), falls into a block that does `mov w3, #0x9` and builds the error. Forcing the permit
 * method to YES fires but does not clear the error (still made at the same site), which means a second
 * accountsd instance or calls that reach it another way. So this is taken at the method: with
 * HAKIOS_BYPASS_ACCOUNT_FILTER=1 the filter forwards straight to its backingAccountStore, which does the
 * fetch without the client-permission gate.
 */
static void (*old_filter_accounts)(id, SEL, void *, void *, void *);

static void
new_filter_accounts(id self, SEL cmd, void *type, void *options, void *completion)
{
	const char *on = knob("HAKIOS_BYPASS_ACCOUNT_FILTER");
	static int told;

	if (on != NULL && *on != '0') {
		id backing = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("backingAccountStore"));
		SEL sel = sel_registerName("accountsWithAccountType:options:completion:");

		if (backing != NULL && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(backing,
		    sel_registerName("respondsToSelector:"), sel)) {
			if (told < 3) {
				told++;
				say("hakios: store(%s): the account filter is bypassed; the backing store "
				    "answers directly\n", who_i_am());
			}
			((void (*)(id, SEL, void *, void *, void *))objc_msgSend)(backing, sel, type, options,
			    completion);
			return;
		}
	}
	if (old_filter_accounts != NULL) {
		old_filter_accounts(self, cmd, type, options, completion);
	}
}

/*
 * The store launches, has an account, cookies, a completed bag fetch -- and never presents content. Its
 * own log repeats AMSSyncAccountFlagsTask: Failed and AMSSyncRestrictedRegionTask: Failed: the account's
 * flags and region are synced from Apple's server for a fabricated account the server does not know, so
 * they fail, and AppStoreKit's bootstrap does not leave onboarding for presentingContent. These answer
 * performSync with an already-finished promise, so the bootstrap treats the sync as done and goes on --
 * where it then makes the Mescal-signed amp-api request that sapd can sign. HAKIOS_FAKE_FLAG_SYNC=1.
 */
static id (*old_flags_sync)(id, SEL);
static id (*old_region_perform)(id, SEL);

static id
a_finished_promise(void)
{
	Class promise = objc_getClass("AMSPromise");

	if (promise == Nil) {
		return NULL;
	}
	return ((id (*)(Class, SEL, id))objc_msgSend)(promise, sel_registerName("promiseWithResult:"), NULL);
}

static id
new_flags_sync(id self, SEL cmd)
{
	const char *on = knob("HAKIOS_FAKE_FLAG_SYNC");
	static int told;

	if (on == NULL || *on == '0') {
		return old_flags_sync != NULL ? old_flags_sync(self, cmd) : NULL;
	}
	if (told < 3) {
		told++;
		say("hakios: store(%s): AMSSyncAccountFlagsTask answered finished, without the server\n",
		    who_i_am());
	}
	return a_finished_promise();
}

static id
new_region_perform(id self, SEL cmd)
{
	const char *on = knob("HAKIOS_FAKE_FLAG_SYNC");
	static int told;

	if (on == NULL || *on == '0') {
		return old_region_perform != NULL ? old_region_perform(self, cmd) : NULL;
	}
	if (told < 3) {
		told++;
		say("hakios: store(%s): AMSSyncRestrictedRegionTask answered finished, without the server\n",
		    who_i_am());
	}
	return a_finished_promise();
}

static void (*old_add_absinthe)(id, SEL, void *, void *, void *, BOOL);
static void (*old_add_storefront_header)(id, SEL, void *, void *, void *, void *);

static void
new_add_storefront_header(id self, SEL cmd, void *request, void *account, void *media_type, void *bag)
{
	char text[300] = "(no URL)";
	id url = request != NULL ? ((id (*)(id, SEL))objc_msgSend)((id)request, sel_registerName("URL")) : NULL;

	keep_bag((id)bag, "the storefront header step");
	old_add_storefront_header(self, cmd, request, account, media_type, bag);
	if (url != NULL) {
		text_of(url, text, sizeof(text));
	}
	say("hakios: store: STOREFRONT header step for %.140s, bag %s\n", text, bag != NULL ? "given" : "(nil)");
	if (strstr(text, "apiToken") != NULL && old_add_absinthe != NULL) {
		void *use = bag != NULL ? bag : (void *)seen_bag;

		say("hakios: store: the token request: asking the absinthe step for a signature (bag %s)\n",
		    use != NULL ? "in hand" : "STILL NONE");
		old_add_absinthe((id)objc_getClass("AMSURLRequestDecoration"),
		    sel_registerName("addAbsintheHeadersToRequest:buyParams:bag:shouldUseRemoteSigningIfAvailable:"),
		    request, NULL, use, NO);
	}
}

/* where a bag is handed over for a request that has one: the GUID step, which names the lack of one */
static void (*old_add_guid)(id, SEL, void *, void *, BOOL);
static void (*old_add_guid_promise)(id, SEL, void *, void *, BOOL);

/*
 * Neither of the two GUID decorations is where "Skipping GUID parameter addition as bag was not provided"
 * comes from: both are taken and never entered, so the line is said somewhere else (Swift, most likely).
 * They are kept, and say so, rather than being tried again.
 */
static void
new_add_guid_promise(id self, SEL cmd, void *request, void *promise, BOOL preserve)
{
	id url = request != NULL ? ((id (*)(id, SEL))objc_msgSend)((id)request, sel_registerName("URL")) : NULL;
	char text[300] = "(no URL)";

	if (url != NULL) {
		text_of(url, text, sizeof(text));
	}
	say("hakios: store: a request is being built: %.200s (bag promise %s)\n", text,
	    promise != NULL ? "given" : "NOT given");
	old_add_guid_promise(self, cmd, request, promise, preserve);
}

static void
new_add_guid(id self, SEL cmd, void *request, void *bag, BOOL preserve)
{
	/*
	 * Which request is being built, and whether it has a bag. AMS repeats "Skipping GUID parameter
	 * addition as bag was not provided" without saying for what, and the request it is talking about
	 * is the one the App Store is trying to make -- so it is named here.
	 */
	id url = request != NULL ? ((id (*)(id, SEL))objc_msgSend)((id)request, sel_registerName("URL")) : NULL;
	char text[300] = "(no URL)";

	if (url != NULL) {
		text_of(url, text, sizeof(text));
	}
	say("hakios: store: a request is being built: %.200s (bag %s)\n", text, bag != NULL ? "given" : "NOT given");
	keep_bag((id)bag, "the GUID step");
	old_add_guid(self, cmd, request, bag, preserve);
}

/*
 * ... and the two that take a URL to match against as well, which are the ones AMS really uses here:
 * "Skipping GUID parameter addition as bag was not provided" is said over and over in this port's App
 * Store while addGUIDParameterToRequest:bag:preservingQueryEncoding: is never entered, so the line comes
 * from these. They name the request the store is building when it has no bag -- the request that never
 * goes out.
 */
static void (*old_add_guid_matching)(id, SEL, void *, void *, void *, BOOL);
static void (*old_add_guid_header_matching)(id, SEL, void *, void *, void *);

static void
say_request(const char *what, void *request, void *url_matched, void *bag)
{
	id url = request != NULL ? ((id (*)(id, SEL))objc_msgSend)((id)request, sel_registerName("URL")) : NULL;
	char text[300] = "(no URL)", matched[200] = "(none)";

	if (url != NULL) {
		text_of(url, text, sizeof(text));
	}
	if (url_matched != NULL) {
		text_of((id)url_matched, matched, sizeof(matched));
	}
	say("hakios: store: %s %.200s (matching %.120s, bag %s)\n", what, text, matched,
	    bag != NULL ? "given" : "NOT given");
	keep_bag((id)bag, what);
}

static void
new_add_guid_matching(id self, SEL cmd, void *request, void *url_matched, void *bag, BOOL preserve)
{
	say_request("a request wants a GUID parameter:", request, url_matched, bag);
	old_add_guid_matching(self, cmd, request, url_matched, bag, preserve);
}

static void
new_add_guid_header_matching(id self, SEL cmd, void *request, void *url_matched, void *bag)
{
	say_request("a request wants a GUID header:", request, url_matched, bag);
	old_add_guid_header_matching(self, cmd, request, url_matched, bag);
}

/*
 * EVERY BAG THIS PROCESS ASKS FOR, by profile. The App Store fetches exactly one bag in a run -- product
 * AppStore, profile adprivacyd -- and that one is AdPlatforms'. Its own never goes out, and a JetEngine
 * JetpackLoader is built with a bag, so a store with no bag asks for no JetPack and no page. Everything
 * goes through +[AMSBag _bagForProfile:profileVersion:processInfo:accountProvider:loadURLOverlay:], so
 * the profiles are named here: a profile asked for and never fetched is a different fault from a profile
 * nobody ever asks for.
 */
static id (*old_bag_for_profile)(id, SEL, void *, long, void *, void *, void *);

static id
new_bag_for_profile(id self, SEL cmd, void *profile, long version, void *process, void *provider,
    void *overlay)
{
	char text[80] = "(none)";
	id bag;

	if (profile != NULL) {
		text_of((id)profile, text, sizeof(text));
	}
	bag = old_bag_for_profile(self, cmd, profile, version, process, provider, overlay);
	say("hakios: store: a bag is asked for: profile %.60s version %ld -> %s\n", text, version,
	    bag != NULL ? "a bag" : "nothing");
	return bag;
}

static void
new_add_absinthe(id self, SEL cmd, void *request, void *buy_params, void *bag, BOOL remote_signing)
{
	char urltext[260] = "(no request)";
	id url = request != NULL ? ((id (*)(id, SEL))objc_msgSend)((id)request, sel_registerName("URL")) : NULL;

	if (url != NULL) {
		text_of(url, urltext, sizeof(urltext));
	}
	if (bag == NULL && seen_bag != NULL) {
		say("hakios: store: ABSINTHE step for %.120s had NO bag: handed the kept one\n", urltext);
		bag = (void *)seen_bag;
	} else {
		say("hakios: store: ABSINTHE step for %.120s, bag %s, remote signing %d\n", urltext,
		    bag != NULL ? "given" : "(nil) and none kept", remote_signing ? 1 : 0);
	}
	old_add_absinthe(self, cmd, request, buy_params, bag, remote_signing);
}

static id
new_data_task_promise(id self, SEL _cmd, id request)
{
	id promise;
	char buffer[200], urltext[260];
	id url = request != NULL ? ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("URL")) : NULL;

	entered[COUNT_dataTaskPromise]++;
	if (url != NULL && strstr(text_of(url, urltext, sizeof(urltext)), "apiToken") != NULL) {
		if (anisette_cmd != NULL) {
			id mut = ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("mutableCopy"));

			if (mut != NULL) {
				old_addAnisette(mut, anisette_cmd, anisette_account, anisette_type, anisette_bag);
				say("hakios: store: INJECTED device anisette into the apiToken request (the store's own account/type/bag)\n");
				request = mut;
			}
		} else {
			say("hakios: store: apiToken request seen but no anisette args captured yet; sent as is\n");
		}
	}
	promise = old_data_task_promise(self, _cmd, request);
	returned[COUNT_dataTaskPromise]++;
	say("hakios: store: STEP dataTaskPromiseWithRequest: -> %s\n", text_of(promise, buffer, sizeof(buffer)));
	return promise;
}

static id
new_data_task(id self, SEL _cmd, id request)
{
	id task;
	char buffer[200];

	entered[COUNT_dataTask]++;
	task = old_data_task(self, _cmd, request);
	returned[COUNT_dataTask]++;
	say("hakios: store: STEP dataTaskWithRequest: -> %s\n", text_of(task, buffer, sizeof(buffer)));
	return task;
}

/*
 * What the request promise waits on. _createRequestWithQueryItems: hands back an AMSMutablePromise
 * and nothing below it is ever reached, so the thread is sitting in one of the steps that finish the
 * request: the cookies (which come from amsaccountsd), the storefront header, the anisette headers
 * (which come from AuthKit through akd, and this device has never been provisioned), or the
 * session's own preparation. Each is entered and left under its own number; the one that is entered
 * and not left is the answer. Arguments are forwarded untouched and never looked at: an anisette
 * "type" is a scalar and _prepareRequest: takes an NSError **, and a description of either would be
 * a crash, not a measurement.
 */
#define WRAP4(name) \
static void *(*old_##name)(id, SEL, void *, void *, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b, void *c, void *d) \
{ \
	unsigned n = step_in(COUNT_##name); \
	void *result = old_##name(self, cmd, a, b, c, d); \
	step_out(COUNT_##name, n, result != NULL); \
	return result; \
}

#define WRAP3(name) \
static void *(*old_##name)(id, SEL, void *, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b, void *c) \
{ \
	unsigned n = step_in(COUNT_##name); \
	void *result = old_##name(self, cmd, a, b, c); \
	step_out(COUNT_##name, n, result != NULL); \
	return result; \
}

#define WRAP1(name) \
static void *(*old_##name)(id, SEL, void *); \
static void * \
new_##name(id self, SEL cmd, void *a) \
{ \
	unsigned n = step_in(COUNT_##name); \
	void *result = old_##name(self, cmd, a); \
	say("hakios: store: #%u is for promise %p\n", n, (void *)self); \
	step_out(COUNT_##name, n, result != NULL); \
	return result; \
}

/* counters only, for the ones that would flood the console: the number is the measurement */
#define WRAPQ1(name) \
static void *(*old_##name)(id, SEL, void *); \
static void * \
new_##name(id self, SEL cmd, void *a) \
{ \
	entered[COUNT_##name]++; \
	void *result = old_##name(self, cmd, a); \
	returned[COUNT_##name]++; \
	return result; \
}
#define WRAPQ2(name) \
static void *(*old_##name)(id, SEL, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b) \
{ \
	entered[COUNT_##name]++; \
	void *result = old_##name(self, cmd, a, b); \
	returned[COUNT_##name]++; \
	return result; \
}
#define WRAPQ3(name) \
static void *(*old_##name)(id, SEL, void *, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b, void *c) \
{ \
	entered[COUNT_##name]++; \
	void *result = old_##name(self, cmd, a, b, c); \
	returned[COUNT_##name]++; \
	return result; \
}

/* the funnel every finished promise goes through, and the two ways of waiting on one */
/*
 * IS FAIRPLAY LOAD-BEARING FOR BROWSING AT ALL? The root of the jam is outside this daemon --
 * fairplayd.H2 starts, leaves at once and is restarted every ten seconds, and an AMS thread waits
 * for a reply that never comes (src/inject_fairplay.c). Before reading an obfuscated daemon to find
 * out why it quits, it is worth knowing whether the store needs its answer in the first place. The
 * top of that stack is an Objective-C method -- -[AMSUserAgent _sharedComponentFairPlayDeviceType],
 * a dispatch_once-memoised lookup that goes down to AMS::FairPlayProvider::getDeviceType() and
 * _AMSFairPlayGetHardwareInfo -- so it costs a method_setImplementation, the same move as the
 * storefront getters.
 * The value is deliberately NOT plausible. A zero or an empty string would travel into the user
 * agent and the Mescal path and read like data; this reads like what it is, and the log says on the
 * same line that it was injected, so that nobody later mistakes it for a measurement.
 * Why the wait looks infinite rather than failed, which is worth keeping in mind: launchd holds a
 * job's MachService port whether or not the job is running, so a client's message is QUEUED, not
 * refused. A daemon that is absent fails fast; a daemon launchd keeps restarting makes its clients
 * wait for ever. That is the README's "look at what launchd restarts" bullet seen from the client.
 */
#define FAIRPLAY_DEVICE_TYPE "REDWOOD-INJECTED-NOT-A-FAIRPLAY-DEVICE"

static CFStringRef fairplay_device_type;

static id (*old_fpDeviceType)(id, SEL);
static id
new_fpDeviceType(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	entered[COUNT_fpDeviceType]++;
	returned[COUNT_fpDeviceType]++;
	say("hakios: store: FairPlay device type answered INJECTED \"" FAIRPLAY_DEVICE_TYPE "\" "
	    "(not measured; the real one blocks for ever, see src/inject_fairplay.c)\n");
	return (id)fairplay_device_type;
}

/*
 * THE WAIT THAT MATTERS, and the one thing this library changes besides answering a storefront.
 * Resolved from the threads' own frames:
 *   -[AMSBagNetworkTask performFetch]_block_invoke
 *     -> -[AMSBagActiveAccountProvider bagStorefrontForAccountMediaType:]
 *       -> -[AMSBagActiveAccountProvider _bagAccountForAccountMediaType:mustContainStorefront:]
 *         -> -[ACAccountStore(AppleMediaServices_Project) _ams_localiTunesAccountAfterUpdatingStorefront:]
 *           -> -[AMSPromise resultWithError:] -> NSConditionLock -> waits for ever
 * and the promise it waits for would be finished by the continuation of
 * _fetchLocaliTunesAccountForAccountType:shouldUpdateStorefront:, which runs
 * _updateStorefrontInLocalAccount: -- itself blocked in resultWithError: on another thread. The
 * local account has no storefront to update and nothing can give it one without the bag, so the
 * thread that would fetch the bag waits for an account update that waits for the bag.
 * So this answers it at once, with the local account as it is: no update, no promise, no wait. The
 * storefront that account answers with is ours, from the getter above.
 */
static id (*old_afterUpdating)(id, SEL, void *);
static id
new_afterUpdating(id self, SEL cmd, void *storefront)
{
	id account;

	(void)storefront;
	entered[COUNT_afterUpdating]++;
	account = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("ams_localiTunesAccount"));
	returned[COUNT_afterUpdating]++;
	say("hakios: store: _ams_localiTunesAccountAfterUpdatingStorefront: answered at once, %s\n",
	    account != NULL ? "with the local account" : "with nothing");
	return account;
}

/*
 * The same wait on iOS 26/27, where that selector is gone: AppleMediaServices asks
 * -[ACAccountStore _ams_localiTunesAccountForAccountMediaType:updateStorefront:] instead. Without it
 * answered, appstored stops at "[Bag] Fetching current account" with the account (null) and never
 * speaks again, so the App Store's XPC session to it is cancelled and the screen stays on Loading
 * (logs/121112-run.log.live in hakios-sm7325). Answered the same way -- the local account as it is, no
 * storefront update, no wait -- but inside a promise that is already finished: on iOS 26/27 this answers
 * a promise, not the account, and its caller sends the answer -addSuccessBlock:, which an ACAccount does
 * not know (the App Store died of that selector, logs/122602-run.log.live). +[AMSPromise promiseWithResult:]
 * is the finished one.
 */
static id (*old_accountForMediaType)(id, SEL, void *, BOOL);
static id
new_accountForMediaType(id self, SEL cmd, void *media_type, BOOL update_storefront)
{
	Class promise_class = objc_getClass("AMSPromise");
	id account, promise;

	(void)cmd, (void)media_type, (void)update_storefront;
	entered[COUNT_afterUpdating]++;
	/*
	 * AMS FIRST, NOW THAT THERE IS AN ACCOUNT TO FIND (HAKIOS_ACCOUNT_PASSTHROUGH=0 puts it back).
	 *
	 * This answer was written for a phone with no account at all, where the original waited for one that
	 * was never coming. With the account types made, that is no longer the shape: with every hook of ours
	 * off, AMS finds its own local account and complains only that it has no storefront -- so the
	 * original now returns something, and ours returning nil in front of it is what the App Store has
	 * been seeing. So the original is asked first and its answer passed straight through; ours is only
	 * for when it has none.
	 */
	{
		const char *through = knob("HAKIOS_ACCOUNT_PASSTHROUGH");

		/* off unless asked for: with it on, amsaccountsd took two uncaught exceptions in the first run
		 * (-[ACAccount continueWithBlock:], the two-shapes problem again, now on THEIR account), and a
		 * daemon that dies is not a default */
		if (through != NULL && *through != '0' && old_accountForMediaType != NULL) {
			id theirs = old_accountForMediaType(self, cmd, media_type, update_storefront);
			static bool said;

			if (theirs != NULL) {
				/*
				 * ... and the account AMS hands back is sent -continueWithBlock: by one of its
				 * callers, exactly as ours was: amsaccountsd died of that selector twice in the
				 * first run of this passthrough. So the same lesson applies to their account as
				 * to ours, and it is taught here rather than only where ours is made.
				 */
				teach_account_to_be_a_promise();
				if (!said) {
					said = true;
					say("hakios: store: _ams_localiTunesAccountForAccountMediaType: answered by "
					    "AMS itself, not by us\n");
				}
				returned[COUNT_afterUpdating]++;
				return theirs;
			}
			if (!said) {
				said = true;
				say("hakios: store: AMS itself answered nothing for the account; ours stands in\n");
			}
		}
	}
	account = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("ams_localiTunesAccount"));
	if (account == NULL) {
		const char *fake = knob("HAKIOS_FAKE_ACCOUNT");

		if (fake != NULL && *fake != '0') {
			account = fabricated_account(self);
		}
	} else {
		/* here, where AMS really hands the account out -- not in local_account_of(), which the two
		 * activeiTunesAccount hooks reach and this path does not */
		put_storefront_on(self, account);
	}
	promise = promise_class != Nil ? ((id (*)(id, SEL, id))objc_msgSend)((id)promise_class,
	    sel_registerName("promiseWithResult:"), account) : NULL;
	returned[COUNT_afterUpdating]++;
	say("hakios: store: _ams_localiTunesAccountForAccountMediaType:updateStorefront: answered at once with a "
	    "finished promise, %s (promise %s)\n", account != NULL ? "the local account" : "nothing",
	    promise != NULL ? "made" : "NOT MADE: no AMSPromise");
	return promise;
}

/*
 * The synchronous wait itself. Four of the daemon's eleven threads sit in
 * -[AMSBagValue valueWithError:] -> -[AMSPromise resultWithError:] -> resultBeforeDate:error: ->
 * NSConditionLock, which is a thread blocked until the bag loads. Who does that, and whether one of
 * them is on the bag's own load path, is the difference between "the store is waiting" and "the bag
 * load is waiting on the bag". These are entered and not left on purpose, so entered > returned here
 * is the measurement, not a fault.
 */
static void *(*old_bagValue)(id, SEL, void *);
static void *
new_bagValue(id self, SEL cmd, void *error)
{
	char frames[10 * 20 + 1] = "";
	void *result;

	char which[120];

	/*
	 * WHICH KEY, and whether it came back. The store asks the bag for its own configuration, and a
	 * key the bag has not got is answered "Bag Value Missing" -- appstored already logs two of
	 * those. If what tells the app where to fetch its content is such a key, then nothing asking
	 * for content is exactly what one would see.
	 */
	entered[COUNT_bagValue]++;
	frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
	result = old_bagValue(self, cmd, error);
	returned[COUNT_bagValue]++;
	/*
	 * The key, and NOT by asking the AMSBagValue to describe itself. That is what the first version
	 * did, and it aborted both daemons: appstored went from one start a session to five, each
	 * ending in SIGABRT through std::terminate, from the run that introduced it (075028) onwards.
	 * -description on this object at this moment raises, nothing catches it, and the process dies.
	 * A plain -key is an NSString and safe to print; if there is none, the key is simply unknown.
	 */
	{
		SEL key_sel = sel_registerName("key");
		id key = class_getInstanceMethod(object_getClass(self), key_sel) != NULL ?
		    ((id (*)(id, SEL))objc_msgSend)(self, key_sel) : NULL;

		note_bagkey(key != NULL ? text_of(key, which, sizeof(which)) : "(no key)", result != NULL);
	}
	return result;
}

static void *(*old_resultBefore)(id, SEL, void *, void *);
static void *
new_resultBefore(id self, SEL cmd, void *date, void *error)
{
	char frames[10 * 20 + 1] = "";
	void *result;

	entered[COUNT_resultBefore]++;
	frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
	note_waiter((void *)self, frames, true);
	result = old_resultBefore(self, cmd, date, error);
	returned[COUNT_resultBefore]++;
	return result;
}

/* this one says which promise, because one promise in seventeen is finished and we want its name */
static void *(*old_finishFunnel)(id, SEL, void *, void *, void *);
/*
 * Every AMS promise fulfils through this funnel; a dataTask promise fulfils with an AMSURLResponse
 * (it answers -data and -response). Catch the App Store media-account auth responses
 * (auth.itunes.apple.com / otp) and print the body, which no parser hook saw -- AMS consumes the
 * data through an NSURLSession delegate, not a completion handler.
 */
/* Print a small structured body (JSON '{', XML/plist '<', or binary 'bplist'); used to surface the
 * short media-auth response that no URL-keyed hook could reach. */
static void
log_small_body(id data)
{
	long len = ((long (*)(id, SEL))objc_msgSend)(data, sel_registerName("length"));
	const void *bytes = len > 0 ? ((const void *(*)(id, SEL))objc_msgSend)(data, sel_registerName("bytes")) : NULL;
	const unsigned char *b = bytes;

	if (b == NULL || len <= 0 || len >= 600) {
		return;
	}
	if (b[0] != '{' && b[0] != '<' && !(len > 6 && memcmp(b, "bplist", 6) == 0)) {
		return;
	}
	{
		char body[640];
		int n = (int)len;

		memcpy(body, b, (size_t)n);
		body[n] = 0;
		for (int i = 0; i < n; i++) {
			if (body[i] == '\n' || body[i] == '\r' || body[i] < 9) {
				body[i] = ' ';
			}
		}
		say("hakios: store: small-body len=%ld %s\n", len, body);
	}
}

static void
maybe_log_response(id result)
{
	SEL respondsSel = sel_registerName("respondsToSelector:");

	if (result == NULL) {
		return;
	}
	/* Case 1: the result is raw NSData -- log it if it is a small structured body. */
	if (((char (*)(id, SEL, SEL))objc_msgSend)(result, respondsSel, sel_registerName("bytes"))
	    && ((char (*)(id, SEL, SEL))objc_msgSend)(result, respondsSel, sel_registerName("length"))) {
		log_small_body(result);
		return;
	}
	/* Case 2: the result wraps a body under -data (AMSURLResponse-like). */
	if (((char (*)(id, SEL, SEL))objc_msgSend)(result, respondsSel, sel_registerName("data"))) {
		id data = ((id (*)(id, SEL))objc_msgSend)(result, sel_registerName("data"));

		if (data != NULL
		    && ((char (*)(id, SEL, SEL))objc_msgSend)(data, respondsSel, sel_registerName("bytes"))) {
			log_small_body(data);
		}
	}
}

static void *
new_finishFunnel(id self, SEL cmd, void *a, void *b, void *c)
{
	void *result;

	entered[COUNT_finishFunnel]++;
	note_finished((void *)self);
	maybe_log_response((id)a);
	result = old_finishFunnel(self, cmd, a, b, c);
	returned[COUNT_finishFunnel]++;
	return result;
}
WRAPQ2(finishResultError)
static void *(*old_thenWithBlock)(id, SEL, void *);
static void *
new_thenWithBlock(id self, SEL cmd, void *a)
{
	void *result;

	char frames[10 * 20 + 1] = "";

	entered[COUNT_thenWithBlock]++;
	frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
	note_waiter((void *)self, frames, false);
	result = old_thenWithBlock(self, cmd, a);
	returned[COUNT_thenWithBlock]++;
	return result;
}
static void *(*old_continueWithBlock)(id, SEL, void *);
static void *
new_continueWithBlock(id self, SEL cmd, void *a)
{
	void *result;

	char frames[10 * 20 + 1] = "";

	entered[COUNT_continueWithBlock]++;
	frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
	note_waiter((void *)self, frames, false);
	result = old_continueWithBlock(self, cmd, a);
	returned[COUNT_continueWithBlock]++;
	return result;
}

WRAP1(finishWithResult)
WRAP1(finishWithError)
WRAP3(addCookies)
WRAP3(addStorefrontHeader)
/* the store's own anisette-adder: run it, and keep the account, media type and bag it was given, so
 * new_data_task_promise can give the token request the very same (the account and bag are retained,
 * since the store releases its own after the call and the token request comes later). */
static void *
new_addAnisette(id self, SEL cmd, void *a, void *b, void *c)
{
	unsigned n = step_in(COUNT_addAnisette);
	void *result = old_addAnisette(self, cmd, a, b, c);

	step_out(COUNT_addAnisette, n, result != NULL);
	if (a != NULL) {
		CFRetain((CFTypeRef)a);
	}
	if (c != NULL) {
		CFRetain((CFTypeRef)c);
	}
	anisette_cmd = cmd;
	anisette_account = a;
	anisette_type = b;
	anisette_bag = c;
	return result;
}

/* _prepareRequest finalises every AMS request (headers, signature) and returns it; rewrite the
 * returned request's URL to the proxy so the GET verify and the POST finish/verify all route there. */
static void *(*old_prepareRequest)(id, SEL, void *, void *, void *);
static void *
new_prepareRequest(id self, SEL cmd, void *a, void *b, void *c)
{
	void *result = old_prepareRequest(self, cmd, a, b, c);

	return (void *)rewrite_to_proxy(result);
}
WRAP4(addCookies4)

/*
 * THE TEST THAT SETTLES LOCAL-VERSUS-WALL. The storefront API token request goes out with no
 * authorization at all (the header dump above), because StoreServices only signs a request that has
 * an account behind it, and this device has none. StoreServices nonetheless carries the primitive
 * that would do it -- SSVAnisetteAddHeadersToURLRequest -- which puts the device's own anisette on a
 * request, the same kind of credential Apple's servers already accepted from this phone at Apple-ID
 * sign-in. SSURLRequestProperties.copyURLRequest is where StoreServices hands out the finished
 * NSURLRequest just before it is sent, so this takes the one headed for apiToken, makes it mutable,
 * and runs that primitive on it. A 200 with a token then means the 401 was only our silence and the
 * store is reachable; a 401 still (or a failure from the primitive) means the token wants a
 * FairPlay/Mescal signature this stub-FairPlay, Secure-Enclave-less phone cannot make, which is the
 * same wall activation meets. The call is guarded to the apiToken URL alone and to a non-null
 * primitive, so nothing else on the path is touched.
 */
static id (*old_copyURLRequest)(id, SEL);
static id
new_copyURLRequest(id self, SEL _cmd)
{
	id request = old_copyURLRequest(self, _cmd);
	char urltext[260];
	id url;

	if (request == NULL || ssv_anisette == NULL) {
		return request;
	}
	url = ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("URL"));
	if (url != NULL && strstr(text_of(url, urltext, sizeof(urltext)), "apiToken") != NULL) {
		id mut = ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("mutableCopy"));

		if (mut != NULL) {
			id error = NULL;

			ssv_anisette(mut, &error);
			say("hakios: store: ran SSVAnisetteAddHeadersToURLRequest on the apiToken request (error %p)\n",
			    (void *)error);
			CFRelease((CFTypeRef)request);	/* copyURLRequest returned +1; hand the caller mut's +1 instead */
			return mut;
		}
	}
	return request;
}

/* ---- installing them --------------------------------------------------------------------------- */

/*
 * THE MEDIA-ACCOUNT 2FA FIX (the real process). The App Store media sign-in reaches 2FA but cannot
 * complete it: AMS, building the final request, asks -[ISAMSBagShim arrayForKey:@"absinthe-requests"]
 * for the rules that say which requests need a device (Absinthe) signature, and this process's bag
 * shim has no such key -> AMSErrorDomain 204 "No bag value for key: absinthe-requests" -> "could not
 * be verified". The key lives in the device's general AMS bag; it is only missing from the shim the
 * auth path consults here. So when the shim returns nil for it, hand back the real value
 * (ABSINTHE_REQUESTS_JSON): AMS matches the authenticate URL against the purchase/eligibility rules,
 * matches none, needs no Absinthe signature, and the request proceeds. (The same hook is in inject_kc
 * for itunesstored; measurement showed the failing arrayForKey call is actually in this store process,
 * amsaccountsd, where the error is also seen with the "store:" prefix.)
 */
static id g_absinthe_requests_store;

static id
absinthe_requests_store(void)
{
	if (g_absinthe_requests_store == NULL) {
		id s = ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"),
		    sel_registerName("stringWithUTF8String:"), ABSINTHE_REQUESTS_JSON);
		id data = s != NULL ? ((id (*)(id, SEL, unsigned long))objc_msgSend)(s,
		    sel_registerName("dataUsingEncoding:"), 4) : NULL;
		id err = NULL;
		id arr = data != NULL ? ((id (*)(id, SEL, id, unsigned long, id *))objc_msgSend)(
		    (id)objc_getClass("NSJSONSerialization"),
		    sel_registerName("JSONObjectWithData:options:error:"), data, 0, &err) : NULL;

		if (arr != NULL) {
			g_absinthe_requests_store = (id)CFRetain((CFTypeRef)arr);
		} else {
			say("hakios: store: absinthe-requests JSON parse FAILED\n");
		}
	}
	return g_absinthe_requests_store;
}

static int g_store_bagseen;

static id (*old_arrayForKey_store)(id, SEL, id);
static id
new_arrayForKey_store(id self, SEL cmd, id key)
{
	id result = old_arrayForKey_store(self, cmd, key);
	char k[80] = "";

	if (key != NULL) {
		CFStringGetCString((CFStringRef)key, k, sizeof(k), kCFStringEncodingUTF8);
	}
	if (key != NULL && g_store_bagseen < 40) {
		g_store_bagseen++;
		say("hakios: store: bag arrayForKey key=\"%s\" -> %s\n", k, result != NULL ? "array" : "NIL");
	}
	if (result == NULL && strcmp(k, "absinthe-requests") == 0) {
		id a = absinthe_requests_store();

		if (a != NULL) {
			say("hakios: store: INJECTED bag absinthe-requests via arrayForKey\n");
			return a;
		}
	}
	return result;
}

static id (*old_bagValue_store)(id, SEL, id, long);
static id
new_bagValue_store(id self, SEL cmd, id key, long valueType)
{
	id result = old_bagValue_store(self, cmd, key, valueType);
	char k[80] = "";

	if (key != NULL) {
		CFStringGetCString((CFStringRef)key, k, sizeof(k), kCFStringEncodingUTF8);
	}
	if (key != NULL && g_store_bagseen < 40) {
		g_store_bagseen++;
		say("hakios: store: bag _bagValueForKey key=\"%s\" type=%ld -> %s\n", k, valueType,
		    result != NULL ? "value" : "NIL");
	}
	if (result == NULL && strcmp(k, "absinthe-requests") == 0) {
		id a = absinthe_requests_store();

		if (a != NULL) {
			say("hakios: store: INJECTED bag absinthe-requests via _bagValueForKey\n");
			return a;
		}
	}
	return result;
}

static struct replacement replacements[] = {
	{ "ISAMSBagShim", "arrayForKey:", false, (IMP)new_arrayForKey_store, (IMP *)&old_arrayForKey_store, false },
	{ "ISAMSBagShim", "_bagValueForKey:valueType:", false, (IMP)new_bagValue_store, (IMP *)&old_bagValue_store, false },
	/* the door every consumer of a storefront goes through */
	{ "ACAccount", "ams_storefront", false, (IMP)new_ams_storefront, (IMP *)&old_ams_storefront, false },
	{ "ACAccount", "ams_storefrontForMediaType:", false, (IMP)new_ams_storefront_media,
	  (IMP *)&old_ams_storefront_media, false },
	{ "ACAccount", "ams_isLocalAccount", false, (IMP)new_is_local_account,
	  (IMP *)&old_is_local_account, false },
	{ "ACAccountStore", "ams_localiTunesAccount", false, (IMP)new_local_itunes_account,
	  (IMP *)&old_local_itunes_account, false },
	{ "ACAccountStore", "accountTypeWithAccountTypeIdentifier:", false, (IMP)new_type_with_identifier,
	  (IMP *)&old_type_with_identifier, false },
	{ "ACAccount", "ams_copyStorefrontFromAccount:", false, (IMP)new_copy_storefront, (IMP *)&old_copy_storefront, false },
	/* and the ones particular callers knock at instead */
	{ "AMSBagNetworkDataSource", "cachedStorefront", false, (IMP)new_cached_storefront, (IMP *)&old_cached_storefront, false },
	{ "AMSBagFixedAccountProvider", "bagStorefrontForAccountMediaType:", false, (IMP)new_bag_storefront,
	  (IMP *)&old_bag_storefront, false },
	{ "AMSBagActiveAccountProvider", "bagStorefrontForAccountMediaType:", false, (IMP)new_bag_storefront_active,
	  (IMP *)&old_bag_storefront_active, false },
	/* watched, not replaced */
	{ "AMSBagActiveAccountProvider", "_bagAccountForAccountMediaType:mustContainStorefront:", false,
	  (IMP)new_bag_account, (IMP *)&old_bag_account, false },
	{ "AMSBagNetworkTask", "_performFetchWithAttemptedCount:account:storefront:", false, (IMP)new_perform_fetch,
	  (IMP *)&old_perform_fetch, false },
	{ "AMSBagNetworkTask", "_queryItemsForClientInfo:profile:profileVersion:cookieNames:account:storefront:", false,
	  (IMP)new_query_items, (IMP *)&old_query_items, false },
	{ "AMSURLSession", "performRequest:completion:", false, (IMP)new_perform_request, (IMP *)&old_perform_request, false },
	{ "AMSURLSession", "performRequest:completionBlock:", false, (IMP)new_perform_request_block,
	  (IMP *)&old_perform_request_block, false },
	{ "AMSBagNetworkTask", "_createRequestWithQueryItems:", false, (IMP)new_create_request, (IMP *)&old_create_request,
	  false },
	{ "AMSBagNetworkTask", "URLSessionPromise", false, (IMP)new_session_promise, (IMP *)&old_session_promise, false },
	{ "AMSURLSession", "dataTaskPromiseWithRequest:", false, (IMP)new_data_task_promise, (IMP *)&old_data_task_promise,
	  false },
	{ "AMSURLSession", "dataTaskWithRequest:", false, (IMP)new_data_task, (IMP *)&old_data_task, false },
	{ "NSMutableURLRequest", "ams_addCookiesForAccount:clientInfo:bag:", false, (IMP)new_addCookies,
	  (IMP *)&old_addCookies, false },
	{ "NSMutableURLRequest", "ams_addStoreFrontHeaderForAccount:client:withBag:", false, (IMP)new_addStorefrontHeader,
	  (IMP *)&old_addStorefrontHeader, false },
	{ "NSMutableURLRequest", "ams_addAnisetteHeadersForAccount:type:bag:", false, (IMP)new_addAnisette,
	  (IMP *)&old_addAnisette, false },
	/* where StoreServices hands out the finished request: the one place to sign the token request */
	{ "SSURLRequestProperties", "copyURLRequest", false, (IMP)new_copyURLRequest, (IMP *)&old_copyURLRequest, false },
	{ "AMSURLSession", "_prepareRequest:properties:error:", false, (IMP)new_prepareRequest,
	  (IMP *)&old_prepareRequest, false },
	{ "AMSMutablePromise", "finishWithResult:", false, (IMP)new_finishWithResult, (IMP *)&old_finishWithResult, false },
	{ "AMSMutablePromise", "finishWithError:", false, (IMP)new_finishWithError, (IMP *)&old_finishWithError, false },
	{ "AMSMutablePromise", "_finishWithResult:error:logDuplicateFinishes:", false, (IMP)new_finishFunnel,
	  (IMP *)&old_finishFunnel, false },
	{ "AMSMutablePromise", "finishWithResult:error:", false, (IMP)new_finishResultError,
	  (IMP *)&old_finishResultError, false },
	{ "AMSMutablePromise", "thenWithBlock:", false, (IMP)new_thenWithBlock, (IMP *)&old_thenWithBlock, false },
	{ "AMSMutablePromise", "continueWithBlock:", false, (IMP)new_continueWithBlock, (IMP *)&old_continueWithBlock,
	  false },
	{ "ACAccountStore", "_ams_localiTunesAccountAfterUpdatingStorefront:", false, (IMP)new_afterUpdating,
	  (IMP *)&old_afterUpdating, false },
	{ "ACAccountStore", "_ams_localiTunesAccountForAccountMediaType:updateStorefront:", false,
	  (IMP)new_accountForMediaType, (IMP *)&old_accountForMediaType, false },
	{ "AMSURLRequestDecoration", "addAbsintheHeadersToRequest:buyParams:bag:shouldUseRemoteSigningIfAvailable:",
	  true, (IMP)new_add_absinthe, (IMP *)&old_add_absinthe, false },
	{ "AMSURLRequestDecoration", "addGUIDParameterToRequest:bag:preservingQueryEncoding:", true,
	  (IMP)new_add_guid, (IMP *)&old_add_guid, false },
	{ "AMSURLRequestDecoration", "_addGUIDParameterToRequest:fromPromise:shouldPreserveEncoding:", true,
	  (IMP)new_add_guid_promise, (IMP *)&old_add_guid_promise, false },
	{ "AMSURLRequestDecoration", "addGUIDParameterToRequest:matchingURL:bag:preservingQueryEncoding:", true,
	  (IMP)new_add_guid_matching, (IMP *)&old_add_guid_matching, false },
	{ "AMSURLRequestDecoration", "addGUIDHeaderToRequest:matchingURL:bag:", true,
	  (IMP)new_add_guid_header_matching, (IMP *)&old_add_guid_header_matching, false },
	{ "AMSBag", "_bagForProfile:profileVersion:processInfo:accountProvider:loadURLOverlay:", true,
	  (IMP)new_bag_for_profile, (IMP *)&old_bag_for_profile, false },
	{ "SSURLConnectionRequest", "shouldMescalSign", false, (IMP)new_should_mescal,
	  (IMP *)&old_should_mescal, false },
	{ "AMSURLRequestDecoration", "addStoreFrontHeaderToRequest:forAccount:mediaType:bag:", true,
	  (IMP)new_add_storefront_header, (IMP *)&old_add_storefront_header, false },
	/* HAKIOS_KEEP_REGION_WAIT=1 leaves these alone, to compare the screen with and without the answer */
	{ "AMSSyncRestrictedRegionTask", "storefrontContainsRestrictedRegionsValueFromBag:", true,
	  (IMP)no_restricted_region, &old_restricted_region, false },
	{ "AMSSyncRestrictedRegionTask", "durationOfSyncRestrictedRegionValueFromBag:", true,
	  (IMP)no_restricted_region, NULL, false },
	{ "AMSSyncAccountFlagsTask", "performSync", false, (IMP)new_flags_sync, (IMP *)&old_flags_sync,
	  false },
	{ "AMSSyncRestrictedRegionTask", "performSync", false, (IMP)new_region_perform,
	  (IMP *)&old_region_perform, false },
	{ "AMSDefaults", "enableRemoteSecuritySigning", true, (IMP)no_remote_signing, NULL, false },
	{ "AMSURLRequestEncoder", "enableRemoteSecuritySigning", false, (IMP)no_remote_signing, NULL, false },
	{ "NSXPCConnection", "initWithMachServiceName:options:", false, (IMP)new_xpc_init,
	  (IMP *)&old_xpc_init, false },
	{ "AMSBagActiveAccountProvider",
	  "_handleFetchedLocalAccount:fetchError:mustContainStorefront:mediaType:error:", false,
	  (IMP)new_handle_fetched, (IMP *)&old_handle_fetched, false },
	{ "ACAccountType", "accessKeys", false, (IMP)new_access_keys, (IMP *)&old_access_keys, false },
	{ "ACAccountStore", "accessKeysForAccountType:", false, (IMP)new_access_keys_for,
	  (IMP *)&old_access_keys_for, false },
	{ "AMSBagNetworkTask", "clientInfo", false, (IMP)new_client_info, (IMP *)&old_client_info, false },
	{ "AMSBagNetworkTask", "_URLCookieNamesForProfile:", false, (IMP)new_cookie_names,
	  (IMP *)&old_cookie_names, false },
	{ "AMSCookieService", "getCookiePropertiesForAccount:cookieDatabaseOnly:error:", false,
	  (IMP)new_cookie_props, (IMP *)&old_cookie_props, false },
	{ "AMSDaemonConnection", "cookieServiceProxySyncWithError:", false, (IMP)new_cookie_proxy_sync,
	  (IMP *)&old_cookie_proxy_sync, false },
	{ "AMSDaemonConnection", "accountManagementServiceProxySyncWithError:", false,
	  (IMP)new_account_proxy_sync, (IMP *)&old_account_proxy_sync, false },
	{ "AMSDaemonConnection", "securityServiceProxySyncWithError:", false, (IMP)new_security_proxy_sync,
	  (IMP *)&old_security_proxy_sync, false },
	{ "AMSDaemonConnection", "itfeServiceProxySyncWithError:", false, (IMP)new_itfe_proxy_sync,
	  (IMP *)&old_itfe_proxy_sync, false },
	{ "AMSDaemonConnection", "autoBugCaptureServiceProxySyncWithError:", false, (IMP)new_abc_proxy_sync,
	  (IMP *)&old_abc_proxy_sync, false },
	{ "ACAccountStore", "accountsWithAccountType:", false, (IMP)new_accounts_with_type,
	  (IMP *)&old_accounts_with_type, false },
	{ "ACAccountStore", "accountsWithAccountTypeIdentifier:", false, (IMP)new_accounts_with_type_id,
	  (IMP *)&old_accounts_with_type_id, false },
	/* the asynchronous doors a daemon uses, which end in accountsd and do not come back */
	{ "ACAccountStore", "accountsWithAccountTypeIdentifiers:preloadedProperties:completion:", false,
	  (IMP)new_accounts_ids_props, (IMP *)&old_accounts_ids_props, false },
	{ "ACAccountStore", "_cachedAccountsWithAccountTypeIdentifiers:completion:", false,
	  (IMP)new_cached_accounts_ids, (IMP *)&old_cached_accounts_ids, false },
	{ "ACAccountStore", "accountsWithAccountType:options:completion:", false,
	  (IMP)new_accounts_type_options, (IMP *)&old_accounts_type_options, false },
	{ "ACAccountStore", "visibleTopLevelAccountsWithAccountTypeIdentifiers:completion:", false,
	  (IMP)new_visible_accounts_ids, (IMP *)&old_visible_accounts_ids, false },
	{ "ACAccountStore", "ams_activeiTunesAccount", false, (IMP)new_active_account,
	  (IMP *)&old_active_account, false },
	{ "ACAccountStore", "ams_activeiTunesAccountForMediaType:", false, (IMP)new_active_account_type,
	  (IMP *)&old_active_account_type, false },
	{ "NSMutableURLRequest", "ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies:", false,
	  (IMP)new_addCookiesBlocking, (IMP *)&old_addCookiesBlocking, false },
	{ "ACAccountStore", "_updateStorefrontInLocalAccount:", false, (IMP)new_updateStorefront,
	  (IMP *)&old_updateStorefront, false },
	{ "JPReader", "processStream:signatureVerifier:options:error:", false, (IMP)new_processStream,
	  (IMP *)&old_processStream, false },
	{ "JEBrotliDecoder", "decompressStreamWithAvailableIn:nextIn:availableOut:nextOut:", false, (IMP)new_brotli,
	  (IMP *)&old_brotli, false },
	{ "NSNotificationCenter", "postNotificationName:object:userInfo:", false, (IMP)new_postName,
	  (IMP *)&old_postName, false },
	{ "NSNotificationCenter", "postNotification:", false, (IMP)new_postNotification,
	  (IMP *)&old_postNotification, false },
	{ "AMSUserAgent", "_sharedComponentFairPlayDeviceType", false, (IMP)new_fpDeviceType,
	  (IMP *)&old_fpDeviceType, false },
	{ "AMSBagValue", "valueWithError:", false, (IMP)new_bagValue, (IMP *)&old_bagValue, false },
	{ "AMSPromise", "resultBeforeDate:error:", false, (IMP)new_resultBefore, (IMP *)&old_resultBefore, false },
	{ "__NSURLSessionLocal", "dataTaskWithRequest:completionHandler:", false, (IMP)new_dataTaskLocal,
	  (IMP *)&old_dataTaskLocal, false },
	{ "__NSURLSessionLocal", "dataTaskWithRequest:", false, (IMP)new_dataTaskPlain,
	  (IMP *)&old_dataTaskPlain, false },
	{ "__NSURLSessionLocal", "uploadTaskWithRequest:fromData:completionHandler:", false, (IMP)new_uploadData,
	  (IMP *)&old_uploadData, false },
	{ "__NSURLSessionLocal", "uploadTaskWithRequest:fromData:", false, (IMP)new_uploadData2,
	  (IMP *)&old_uploadData2, false },
	{ "__NSURLSessionLocal", "dataTaskWithURL:completionHandler:", false, (IMP)new_dataTaskURL,
	  (IMP *)&old_dataTaskURL, false },
	{ "AMSURLSession", "dataTaskWithRequest:signpostID:completionHandler:", false, (IMP)new_dataTaskSignpost,
	  (IMP *)&old_dataTaskSignpost, false },
	{ "AMSURLSession", "dataTaskWithRequest:completionHandler:", false, (IMP)new_dataTaskAMS,
	  (IMP *)&old_dataTaskAMS, false },
	{ "NSError", "errorWithDomain:code:userInfo:", true, (IMP)new_errorWith, (IMP *)&old_errorWith, false },
	{ "NSError", "initWithDomain:code:userInfo:", false, (IMP)new_errorInit, (IMP *)&old_errorInit, false },
	{ "NSURLSessionTask", "resume", false, (IMP)new_resume_base, (IMP *)&old_resume_base, false },
	{ "__NSCFLocalSessionTask", "resume", false, (IMP)new_resume_local, (IMP *)&old_resume_local, false },
};

static size_t
replacement_count(void)
{
	return sizeof(replacements) / sizeof(replacements[0]);
}

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	if (watch_only) {
		return;
	}
	hook_fairplay();                /* as soon as AppleMediaServices is there, before it runs */
	hook_bootstrap();               /* ... and trace the store bootstrap phases (AppStore, if asked) */
	hook_ams_log();                 /* ... and the logging call that kills appstored, once its framework is in */
	hook_first_unlock();            /* ... and the question jetpackassetd will not start without */
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (g_absinthe_only && strcmp(r->class_name, "ISAMSBagShim") != 0) {
			continue;               /* amsaccountsd: only the absinthe-requests row, nothing else */
		}
		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		SEL selector = sel_registerName(r->selector);
		Class super = class_getSuperclass(cls);

		method = r->class_method ? class_getClassMethod(cls, selector) : class_getInstanceMethod(cls, selector);
		if (method == NULL) {
			continue;               /* the class is there, the selector is not */
		}
		/*
		 * class_getInstanceMethod walks up: a method this class only INHERITS is the superclass's,
		 * and method_setImplementation on it would replace it for every class that inherits it as
		 * well -- for NSObject, that is every object in the daemon. appstored restarted 31 times in
		 * a run before this check was here, and said nothing on its way out (an ObjC exception ends
		 * in abort(), which the fault reporter did not watch for either; now it does).
		 */
		if (super != NULL && method == (r->class_method ? class_getClassMethod(super, selector) :
		    class_getInstanceMethod(super, selector))) {
			r->done = true;
			say("hakios: store: %c[%s %s] is inherited, not this class's: left alone\n",
			    r->class_method ? '+' : '-', r->class_name, r->selector);
			continue;
		}
		r->done = true;
		original = method_getImplementation(method);
		if (r->original != NULL && *r->original == NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("hakios: store: %c[%s %s] taken (was %#lx unslid)\n", r->class_method ? '+' : '-', r->class_name,
		    r->selector, (uintptr_t)original - slide);
	}
}

__attribute__((constructor))
static void
hakios_store_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	/*
	 * Both of them: appstored holds the bag, but AppStore.app has an AppleMediaServices of its own
	 * and blocks in the same FairPlay lookup -- with the daemon unjammed and the app not, the
	 * daemon fetches its bag and the screen still says LOADING, which is exactly what the first run
	 * of the FairPlay answer showed.
	 */
	/*
	 * (amsaccountsd also carries this library but is left untouched: running even the absinthe-only
	 * path there made the App Store catalog stop connecting, so that experiment is reverted.)
	 */
	/*
	 * And accountsd, but only to be looked at: appstored's account query to it times out after two
	 * seconds and the connection it makes next never returns, while accountsd serves other clients in
	 * the same run. Nothing of ours is replaced there -- this library hooks ACAccountStore, which is
	 * accountsd's own job -- so it only names the services it connects to and dumps its threads.
	 */
	if (name != NULL && strcmp(name, "accountsd") == 0) {
		pthread_t watcher;

		watch_only = true;      /* the periodic report calls image_added again: it must not replace anything */

		slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
		who = name;
		say("hakios: store(%s): ours is in, to watch only: no method of this process is replaced\n", name);
		watch_account_types();
		grant_account_access();
		/*
		 * ... and, when asked, the two doors NSError is made through, so that the refusal the client
		 * reports ("Error returned from daemon: com.apple.accounts Code=9") is named by its maker's
		 * frames. This process replaces nothing else, so these go in by hand.
		 */
		if (knob("HAKIOS_NAME_THE_REFUSAL") != NULL && *knob("HAKIOS_NAME_THE_REFUSAL") != '0') {
			Class error = objc_getClass("NSError");
			Method w = error != Nil ? class_getClassMethod(error,
			    sel_registerName("errorWithDomain:code:userInfo:")) : NULL;
			Method i = error != Nil ? class_getInstanceMethod(error,
			    sel_registerName("initWithDomain:code:userInfo:")) : NULL;

			if (w != NULL) {
				old_errorWith = (id (*)(id, SEL, void *, long, void *))method_getImplementation(w);
				method_setImplementation(w, (IMP)new_errorWith);
			}
			if (i != NULL) {
				old_errorInit = (id (*)(id, SEL, void *, long, void *))method_getImplementation(i);
				method_setImplementation(i, (IMP)new_errorInit);
			}
			say("hakios: store: every error this daemon makes will name its maker\n");
		}
		/*
		 * ... and the refusal is made HERE, inside this daemon, by AMS's own code -- the frames say
		 * so. The table that would have replaced it is not installed in this process, so the one
		 * method that matters goes in by hand.
		 */
		if (knob("HAKIOS_FEED_LOCAL_ACCOUNT") != NULL && *knob("HAKIOS_FEED_LOCAL_ACCOUNT") != '0') {
			one_grant(objc_getClass("AMSBagActiveAccountProvider"),
			    "_handleFetchedLocalAccount:fetchError:mustContainStorefront:mediaType:error:",
			    (IMP)new_handle_fetched, &old_handle_fetched);
			say("hakios: store: a refused local account will be answered with the one that exists\n");
		}
		if (knob("HAKIOS_MAKE_TYPES") != NULL) {
			/* the accessor has to be hooked before the daemon first hands a connection out, which is
			 * long before the thread below wakes up to want one */
			remember_database_connection();
		}
		hook_mach_services();
		if (pthread_create(&watcher, NULL, late_report, NULL) == 0) {
			pthread_detach(watcher);
		}
		return;
	}
	/*
	 * akd (AuthKit), to WATCH ONLY. The App Store bootstrap blocks in a synchronous AMS->AuthKit
	 * authorization (thread 2 in mach_msg forever, com.apple.ak.auth.xpc, with AppSSO -1000 beside it).
	 * Carry the library here to see whether akd is itself blocked -- and on what -- or idle while the
	 * app waits: it names the services akd talks to (HAKIOS_NAME_SERVICES) and dumps akd's threads every
	 * minute. Nothing of ours is replaced in akd; this is a probe, not an answer.
	 */
	if (name != NULL && strcmp(name, "akd") == 0) {
		pthread_t watcher;

		watch_only = true;
		slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
		who = name;
		say("hakios: store(%s): ours is in, to watch only: no method of this process is replaced\n", name);
		hook_mach_services();
		if (pthread_create(&watcher, NULL, late_report, NULL) == 0) {
			pthread_detach(watcher);
		}
		return;
	}
	/*
	 * jetpackassetd as well: the App Store's pages are JetPacks, and JetEngine asks that daemon for them
	 * (it answers the app's "prewarm" with "success" and takes its "initialize"), but the daemon itself
	 * fetches over the network with a bag of its own -- so it needs the same storefront answered, and its
	 * requests and errors are worth the same table as the store's.
	 */
	/*
	 * amsaccountsd as well. "Local account not found" reaches the App Store as an error decoded from an
	 * XPC reply -- it is not made in the app at all, it is what the daemon on the other side answers --
	 * and the daemon the app asks is com.apple.xpc.amsaccountsd. An account made inside the app is
	 * therefore invisible to the question that matters; the answer has to be given where the question
	 * is asked.
	 */
	if (name == NULL || (strcmp(name, "appstored") != 0 && strcmp(name, "AppStore") != 0
	    && strcmp(name, "jetpackassetd") != 0 && strcmp(name, "amsaccountsd") != 0)) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	watch_for_faults();
	name_uncaught_exceptions();
	hook_fairplay();                /* first of all: the one call that never comes back */
	hook_ams_log();                 /* ... and, if its framework is already in, the call that kills appstored */
	hook_first_unlock();            /* ... and the first-unlock question, which MobileKeyBag always answers */
	{
		/*
		 * The storefront, and whether anything is replaced at all, from the phone (see knob()).
		 * HAKIOS_NO_HOOKS=1 makes this process a watcher: the same library, the same lines on the
		 * console, and not one method of the process changed -- the control for every answer this
		 * library gives, without building an image for it.
		 */
		const char *wanted = knob("HAKIOS_STOREFRONT");
		const char *nothing = knob("HAKIOS_NO_HOOKS");

		storefront = CFStringCreateWithCString(NULL, wanted != NULL ? wanted : STOREFRONT,
		    kCFStringEncodingUTF8);
		if (nothing != NULL && *nothing != '0') {
			watch_only = true;
		}
		fairplay_device_type = CFStringCreateWithCString(NULL, FAIRPLAY_DEVICE_TYPE,
		    kCFStringEncodingUTF8);
		who = name;
		ssv_anisette = (ssv_anisette_fn)dlsym(RTLD_DEFAULT, "SSVAnisetteAddHeadersToURLRequest");
		say("hakios: store(%s): ours is in, storefront to answer with is %s, anisette primitive %s%s\n",
		    name, wanted != NULL ? wanted : STOREFRONT, ssv_anisette != NULL ? "found" : "NOT found",
		    watch_only ? ", AND NOTHING IS REPLACED (HAKIOS_NO_HOOKS)" : "");
	}
	hook_xpc_sessions();
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, late_report, NULL) == 0) {
			pthread_detach(thread);
		}
	}
	hook_mach_services();
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
}
