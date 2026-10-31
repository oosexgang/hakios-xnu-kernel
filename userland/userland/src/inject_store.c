/*
 * libredwood-store.dylib: code of ours inside appstored (see inject.c for how it gets there).
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
#include <objc/message.h>
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

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */
#define STOREFRONT "143469-2,8"                 /* what bag.itunes.apple.com returned for this device */

static uintptr_t slide;
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
	if (strncmp(line, "redwood: store: ", 16) == 0) {
		char named[sizeof(line) + 32];

		n = snprintf(named, sizeof(named), "redwood: %s: %s", who, line + 16);
		if (n > (int)sizeof(named) - 1) {
			n = sizeof(named) - 1;
		}
		write(console >= 0 ? console : 2, named, (size_t)n);
		return;
	}
	write(console >= 0 ? console : 2, line, (size_t)n);
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
	say("redwood: store: -> #%u %s\n", n, counted[which]);
	return n;
}

static void
step_out(int which, unsigned n, bool got_something)
{
	returned[which]++;
	say("redwood: store: <- #%u %s%s\n", n, counted[which], got_something ? "" : " (nil)");
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

static void
dump_tasks(void)
{
	static const char *const states[] = { "running", "suspended", "cancelling", "COMPLETED" };

	say("redwood: store: %u tasks resumed\n", task_count);
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

		say("redwood: store: task %u %s status %ld, %lld bytes, %s%s%s, %s\n", i,
		    state >= 0 && state < 4 ? states[state] : "?", status, got,
		    type != NULL ? text_of(type, one, sizeof(one)) : "(no type)",
		    error != NULL ? ", error " : "", error != NULL ? text_of(error, two, sizeof(two)) : "",
		    url != NULL ? text_of(url, three, sizeof(three)) : "(no url)");
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
 *   redwood: AppStore: JETPACK processStream: in
 *   redwood: AppStore: JETPACK processStream: out, a result, error (none)
 *   redwood: AppStore: KEY brotli entered 0 returned 0, processStream entered 2 returned 2
 *   redwood: appstored: KEY brotli entered 0 returned 0, processStream entered 0 returned 0
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
		say("redwood: store: BROTLI decompressStream -> %ld (call %lu)\n", result, returned[COUNT_brotli]);
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
static id (*old_dataTaskLocal)(id, SEL, void *, void *);
static id
new_dataTaskLocal(id self, SEL cmd, void *request, void *handler)
{
	id task;
	char buffer[260];

	entered[COUNT_dataTaskCreated]++;
	task = ((id (*)(id, SEL, void *, void *))old_dataTaskLocal)(self, cmd, request, handler);
	returned[COUNT_dataTaskCreated]++;
	say("redwood: store: TASK CREATED %s\n", text_of((id)request, buffer, sizeof(buffer)));
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
	task = ((id (*)(id, SEL, void *, void *))old_dataTaskAMS)(self, cmd, request, handler);
	returned[COUNT_dataTaskCreated]++;
	say("redwood: store: TASK CREATED (ams session) %s\n", text_of((id)request, buffer, sizeof(buffer)));
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
	say("redwood: store: TASK CREATED (url) %s\n", text_of((id)url, buffer, sizeof(buffer)));
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
	task = ((id (*)(id, SEL, void *, void *, void *))old_dataTaskSignpost)(self, cmd, request, signpost, handler);
	returned[COUNT_dataTaskCreated]++;
	say("redwood: store: TASK CREATED (ams) %s\n", text_of((id)request, buffer, sizeof(buffer)));
	return task;
}

static id (*old_dataTaskPlain)(id, SEL, void *);
static id
new_dataTaskPlain(id self, SEL cmd, void *request)
{
	id task;
	char buffer[260];

	entered[COUNT_dataTaskCreated]++;
	task = ((id (*)(id, SEL, void *))old_dataTaskPlain)(self, cmd, request);
	returned[COUNT_dataTaskCreated]++;
	say("redwood: store: TASK CREATED (no handler) %s\n", text_of((id)request, buffer, sizeof(buffer)));
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
			say("redwood: store: class %s\n", name);
		}
	}
	free(classes);
	say("redwood: store: %u classes in all, %u named like \"%s\"\n", count, shown, needle);
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
	entered[COUNT_updateStorefront]++;
	returned[COUNT_updateStorefront]++;
	say("redwood: store: INJECTED _updateStorefrontInLocalAccount: answered at once, no update\n");
	return (void *)((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("ams_localiTunesAccount"));
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
	say("redwood: store: INJECTED ams_addCookiesForAccount:...:cleanupGlobalCookies: answered with "
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
	say("redwood: store: JETPACK processStream: in\n");
	result = old_processStream(self, cmd, stream, verifier, options, error);
	returned[COUNT_processStream]++;
	{
		/* and who owns the result next: whoever is on the stack when it is handed back */
		char frames[10 * 20 + 1] = "";

		frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
		say("redwood: store: JETPACK processStream: out, %s, error %s, to%s\n",
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
 *   redwood: appstored: signal 6 ... called from _abort, demangling_terminate_handler,
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
		say("redwood: store: NOTIFICATION %s\n", text);
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
		say("redwood: store: NOTIFICATION (object) %s\n", text);
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
	say("redwood: store: %u kinds of error made\n", error_count);
	for (unsigned i = 0; i < error_count; i++) {
		say("redwood: store: error %-40s code %ld, %u time(s)\n", errors[i].domain, errors[i].code,
		    errors[i].count);
	}
}

static id (*old_errorWith)(id, SEL, void *, long, void *);
static id
new_errorWith(id self, SEL cmd, void *domain, long code, void *info)
{
	note_error((id)domain, code);
	return old_errorWith(self, cmd, domain, code, info);
}

static id (*old_errorInit)(id, SEL, void *, long, void *);
static id
new_errorInit(id self, SEL cmd, void *domain, long code, void *info)
{
	note_error((id)domain, code);
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
	say("redwood: store: %u bag keys asked for\n", bagkey_count);
	for (unsigned i = 0; i < bagkey_count; i++) {
		say("redwood: store: bag key %-44s asked %u, answered %u%s\n", bagkeys[i].key, bagkeys[i].asked,
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
	say("redwood: store: %u promises seen\n", promise_count);
	for (unsigned i = 0; i < promise_count; i++) {
		struct promise_note *note = &promises[i];

		say("redwood: store: promise %p %-16s waiters %2u sync %u finished %u%s tid %llu seen from%s\n",
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
		say("redwood: store: task_threads() refused\n");
		return;
	}
	say("redwood: store: %u threads\n", count);
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
			say("redwood: store: thread %u: no state (run %d)\n", i, run_state);
			continue;
		}
		frames_from((uintptr_t)arm_thread_state64_get_fp(state), text, sizeof(text));
		say("redwood: store: thread %u: run %d suspend %d pc %#lx lr %#lx from%s\n", i, run_state, suspend,
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
		pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(context->uc_mcontext->__ss);
		lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(context->uc_mcontext->__ss);
		fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(context->uc_mcontext->__ss);
	}
	frames_from(fp, frames, sizeof(frames));
	say("redwood: store: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
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
		say("redwood: store: ams_storefront was %s, now " STOREFRONT "\n", text_of(was, buffer, sizeof(buffer)));
	}
	returned[COUNT_ams_storefront]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_cached_storefront(id self, SEL _cmd)
{
	entered[COUNT_cachedStorefront]++;
	id was = old_cached_storefront != NULL ? old_cached_storefront(self, _cmd) : NULL;

	say("redwood: store: cachedStorefront was %s\n", was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_cachedStorefront]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_bag_storefront(id self, SEL _cmd, id media_type)
{
	entered[COUNT_bagStorefront_fixed]++;
	id was = old_bag_storefront != NULL ? old_bag_storefront(self, _cmd, media_type) : NULL;
	char buffer[128];

	say("redwood: store: bagStorefrontForAccountMediaType:%s was %s\n", text_of(media_type, buffer, sizeof(buffer)),
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

	say("redwood: store: active bagStorefrontForAccountMediaType:%s was %s\n",
	    text_of(media_type, buffer, sizeof(buffer)), was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_bagStorefront_active]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_copy_storefront(id self, SEL _cmd, id account)
{
	entered[COUNT_copyStorefront]++;
	id was = old_copy_storefront != NULL ? old_copy_storefront(self, _cmd, account) : NULL;

	say("redwood: store: ams_copyStorefrontFromAccount: was %s\n",
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

	say("redwood: store: _bagAccountForAccountMediaType:%s mustContainStorefront:%d -> %s\n",
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

	say("redwood: store: BAG FETCH attempt %ld, storefront %s, account %s\n", attempt,
	    text_of(front, two, sizeof(two)), text_of(account, one, sizeof(one)));
	for (size_t i = 0; i < replacement_count(); i++) {
		if (!replacements[i].done) {
			say("redwood: store: NOT TAKEN %c[%s %s]\n", replacements[i].class_method ? '+' : '-',
			    replacements[i].class_name, replacements[i].selector);
		}
	}
	promise = old_perform_fetch(self, _cmd, attempt, account, front);
	returned[COUNT_performFetch]++;
	note_label(promise, "THE BAG FETCH", NULL);
	say("redwood: store: BAG FETCH returned %s\n", promise != NULL ? "a promise" : "(nil)");
	return promise;
}

static id
new_query_items(id self, SEL _cmd, id client, id profile, id version, id cookies, id account, id front)
{
	entered[COUNT_queryItems]++;
	id items = old_query_items(self, _cmd, client, profile, version, cookies, account, front);
	char one[64], two[160];

	say("redwood: store: BAG QUERY profile %s, storefront %s -> %s\n", text_of(profile, one, sizeof(one)),
	    text_of(front, two, sizeof(two)), items != NULL ? "items" : "(nil)");
	return items;
}

/* the session layer: a request that is built but never sent stops between these and the wire */
static id
new_perform_request(id self, SEL _cmd, id request, id completion)
{
	entered[COUNT_performRequest]++;
	char buffer[320];

	say("redwood: store: AMSURLSession performRequest:completion: %s\n", text_of(request, buffer, sizeof(buffer)));
	return old_perform_request(self, _cmd, request, completion);
}

static id
new_perform_request_block(id self, SEL _cmd, id request, id completion)
{
	entered[COUNT_performRequestBlock]++;
	char buffer[320];

	say("redwood: store: AMSURLSession performRequest:completionBlock: %s\n", text_of(request, buffer, sizeof(buffer)));
	return old_perform_request_block(self, _cmd, request, completion);
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

	say("redwood: store: TASK RESUME (%s) %s\n", where, text_of(request, buffer, sizeof(buffer)));
}

static void
new_resume_base(id self, SEL _cmd)
{
	entered[COUNT_resume]++;
	say_resume(self, "NSURLSessionTask");
	remember_task(self);
	old_resume_base(self, _cmd);
	returned[COUNT_resume]++;
}

static void
new_resume_local(id self, SEL _cmd)
{
	entered[COUNT_resumeLocal]++;
	say_resume(self, "__NSCFLocalSessionTask");
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
		say("redwood: store: %s: no such class\n", class_name);
		return;
	}
	for (unsigned i = 0; i < count; i++) {
		const char *name = sel_getName(method_getName(methods[i]));

		if (needle == NULL || strstr(name, needle) != NULL) {
			say("redwood: store: %s has -%s\n", class_name, name);
		}
	}
	free(methods);
}

static void
one_report(unsigned seconds)
{
	bool first = seconds <= 15;

	say("redwood: store(%s): report at %u s\n", who, seconds);
	image_added(NULL, 0);           /* and try again for anything whose class has turned up since */
	/*
	 * These two on their own, before the long report: last time the brotli counter was one line of
	 * sixty and it went missing, and a missing line is what this branch has been wrong about five
	 * times. A number that matters does not travel in a crowd.
	 */
	say("redwood: store: KEY brotli entered %lu returned %lu, processStream entered %lu returned %lu\n",
	    entered[COUNT_brotli], returned[COUNT_brotli], entered[COUNT_processStream], returned[COUNT_processStream]);

	for (size_t i = 0; i < replacement_count(); i++) {
		if (!replacements[i].done) {
			say("redwood: store: NOT TAKEN %c[%s %s]\n", replacements[i].class_method ? '+' : '-',
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
		say("redwood: store: entered %4lu returned %4lu %s%s\n", entered[i], returned[i], counted[i],
		    entered[i] > returned[i] ? "   <-- STILL IN IT" :
		    entered[i] == 0 ? "   <-- NEVER ENTERED: suspect the hook, not the code" : "");
	}
	dump_promises();
	dump_tasks();
	dump_bagkeys();
	dump_errors();
	dump_threads();
	/* the long dumps once; the numbers in every report, because only a late one survives the log */
	if (first) {
		dump_selectors("AMSURLSession", "equest");
		dump_selectors("JEBrotliDecoder", NULL);
		dump_selectors("_TtGC9JetEngine7PromiseT__", NULL);
		dump_selectors("JetEngine", NULL);
		dump_classes("Jet");
		dump_classes("JE");
	}
	say("redwood: store: %lu notifications posted\n", notifications);
}

/*
 * Every minute rather than once. appstored starts with the system and the App Store starts when
 * somebody taps it, a minute and a half later, so a single report at 150 s falls past the end of
 * the run for one of them -- and the app's side of the story went missing in exactly that way.
 */
static void *
late_report(__unused void *ignored)
{
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
	say("redwood: store: STEP _createRequestWithQueryItems: -> %s\n", text_of(request, buffer, sizeof(buffer)));
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
	say("redwood: store: STEP URLSessionPromise -> %s\n", text_of(promise, buffer, sizeof(buffer)));
	return promise;
}

static id
new_data_task_promise(id self, SEL _cmd, id request)
{
	id promise;
	char buffer[200];

	entered[COUNT_dataTaskPromise]++;
	promise = old_data_task_promise(self, _cmd, request);
	returned[COUNT_dataTaskPromise]++;
	say("redwood: store: STEP dataTaskPromiseWithRequest: -> %s\n", text_of(promise, buffer, sizeof(buffer)));
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
	say("redwood: store: STEP dataTaskWithRequest: -> %s\n", text_of(task, buffer, sizeof(buffer)));
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
	say("redwood: store: #%u is for promise %p\n", n, (void *)self); \
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
	say("redwood: store: FairPlay device type answered INJECTED \"" FAIRPLAY_DEVICE_TYPE "\" "
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
	say("redwood: store: _ams_localiTunesAccountAfterUpdatingStorefront: answered at once, %s\n",
	    account != NULL ? "with the local account" : "with nothing");
	return account;
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
static void *
new_finishFunnel(id self, SEL cmd, void *a, void *b, void *c)
{
	void *result;

	entered[COUNT_finishFunnel]++;
	note_finished((void *)self);
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
WRAP3(addAnisette)
WRAP3(prepareRequest)
WRAP4(addCookies4)

/* ---- installing them --------------------------------------------------------------------------- */

static struct replacement replacements[] = {
	/* the door every consumer of a storefront goes through */
	{ "ACAccount", "ams_storefront", false, (IMP)new_ams_storefront, (IMP *)&old_ams_storefront, false },
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
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

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
			say("redwood: store: %c[%s %s] is inherited, not this class's: left alone\n",
			    r->class_method ? '+' : '-', r->class_name, r->selector);
			continue;
		}
		r->done = true;
		original = method_getImplementation(method);
		if (r->original != NULL && *r->original == NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("redwood: store: %c[%s %s] taken (was %#lx unslid)\n", r->class_method ? '+' : '-', r->class_name,
		    r->selector, (uintptr_t)original - slide);
	}
}

__attribute__((constructor))
static void
redwood_store_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	/*
	 * Both of them: appstored holds the bag, but AppStore.app has an AppleMediaServices of its own
	 * and blocks in the same FairPlay lookup -- with the daemon unjammed and the app not, the
	 * daemon fetches its bag and the screen still says LOADING, which is exactly what the first run
	 * of the FairPlay answer showed.
	 */
	if (name == NULL || (strcmp(name, "appstored") != 0 && strcmp(name, "AppStore") != 0)) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	watch_for_faults();
	storefront = CFStringCreateWithCString(NULL, STOREFRONT, kCFStringEncodingUTF8);
	fairplay_device_type = CFStringCreateWithCString(NULL, FAIRPLAY_DEVICE_TYPE, kCFStringEncodingUTF8);
	who = name;
	say("redwood: store(%s): ours is in, storefront to answer with is " STOREFRONT "\n", name);
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, late_report, NULL) == 0) {
			pthread_detach(thread);
		}
	}
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
}
