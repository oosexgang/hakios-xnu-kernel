/*
 * A device object that implements nothing and says what it was asked for.
 *
 * metal_dev measured the three classes a driver of ours could inherit. The third, the one
 * the real driver uses, answered with Apple's own assertion:
 *
 *   Assertion failed: ([self conformsToProtocol:@protocol(MTLDeviceSPI)]),
 *   function -[IOGPUMetalDevice initWithAcceleratorPort:options:], IOGPUMetalDevice.m:323
 *
 * That is a gate in USERLAND, not in the kernel, and `class_addProtocol` passes it. Which
 * means Apple's own initialisation can be made to run against a class of ours -- and
 * whatever it then asks that class for, it asks here, in a log, in order. That is worth
 * more than any amount of disassembly: this program does not try to be a driver, it tries
 * to be asked.
 *
 * So, at runtime and with nothing written down about Apple's layout:
 *
 *   1. subclass the candidate (argv[1], default IOGPUMetalDevice),
 *   2. add the MTLDeviceSPI and MTLDevice protocols to it, which is what the assertion
 *      reads,
 *   3. for every method those protocols REQUIRE and the chain does not already answer,
 *      install a stub that prints its own selector and returns zero. The stub is chosen by
 *      the method's return type, because a method returning a struct by value is handed a
 *      hidden pointer to the caller's buffer and a stub of the wrong width writes past it.
 *      A return type with no matching stub is left out and named, so the refusal that
 *      follows is attributable,
 *   4. run step 5 of the discovery chain, and let Apple's code interrogate the stub.
 *
 * Optional protocol methods are deliberately NOT stubbed: Apple's code asks
 * respondsToSelector: about those, and answering yes to something we have not implemented
 * would make it take a road the port cannot follow. They are listed instead.
 *
 * Nothing here claims the object works. The deliverable is the order of the questions.
 *
 * The second mode, --minimal, tests a claim instead of collecting questions. Reversing
 * MTLAddDevice (0x181a1875c, the function Metal registers a plugin's object with) gave a
 * contract that is almost nothing: the object must come back non-nil from
 * -initWithAcceleratorPort:, must conform to MTLDevice and MTLDeviceSPI -- both asserted,
 * both fatal -- and must answer -featureProfile and -maxBufferLength. The other 41
 * selectors that -initLimits and -initFeatureQueries send already have a _MTLDevice
 * default. So --minimal builds a subclass with exactly those three methods and the two
 * protocol declarations, nothing else, and then
 *
 *   - replays MTLAddDevice's sequence by hand, so that a missing piece is named by the step
 *     it fails in rather than from inside Apple's code,
 *   - does that once per candidate -featureProfile value, because -initLimits switches on
 *     it and a value this hardware never reports may walk off the end of a table. Each
 *     subclass is separate, so a value that faults costs one line and the sweep goes on,
 *   - then, for the first value that survives, calls Metal's OWN MTLAddDevice on the object
 *     and asks MTLCreateSystemDefaultDevice() for a device.
 *
 * That last step bypasses discovery -- steps 1 to 4, the service properties and the bundle
 * -- and bypasses nothing else: registration, both assertions and the whole limits and
 * feature-query path run as Apple wrote them. If MTLCreateSystemDefaultDevice() then
 * returns the object, what has been shown is that this port can hold a Metal device, and
 * NOT that Metal can find one here. The two are reported separately for that reason.
 *
 * Run: /redwood/bin/metal_stubdev [CLASS] [--minimal]   (RC_JOBS=rc.jobs.metaldev)
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>

#define MTL_CREATE_DEVICE_UNSLID 0x181a34b3cUL
extern id MTLCreateSystemDefaultDevice(void);

static sigjmp_buf recover;
static volatile sig_atomic_t guarded;
static unsigned asked;

static uintptr_t
cache_slide(void)
{
	return (uintptr_t)&MTLCreateSystemDefaultDevice - MTL_CREATE_DEVICE_UNSLID;
}

static void
fault(int sig, siginfo_t *info, void *uap)
{
	ucontext_t *uc = uap;
	uintptr_t slide = cache_slide(), pc = 0, lr = 0;

	if (uc != NULL && uc->uc_mcontext != NULL) {
		pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
		lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
	}
	printf("stubdev: signal %d at %p after %u questions\n", sig,
	       info != NULL ? info->si_addr : NULL, asked);
	printf("stubdev:   pc %#lx unslid, lr %#lx unslid (slide %#lx)\n", pc - slide,
	       lr - slide, slide);
	fflush(stdout);
	if (guarded) {
		guarded = 0;
		siglongjmp(recover, 1);
	}
	_exit(sig);
}

/* Every stub goes through here, so the log is the sequence of questions. */
static void
note(id self, SEL cmd)
{
	printf("stubdev: #%u asked -[%s %s]\n", ++asked, class_getName(object_getClass(self)),
	       sel_getName(cmd));
	fflush(stdout);
}

struct ret16 { unsigned long v[2]; };
struct ret24 { unsigned long v[3]; };
struct ret32 { unsigned long v[4]; };
struct ret48 { unsigned long v[6]; };
struct ret64 { unsigned long v[8]; };

static void stub_v(id s, SEL c) { note(s, c); }
static long stub_i(id s, SEL c) { note(s, c); return 0; }
static float stub_f(id s, SEL c) { note(s, c); return 0.0f; }
static double stub_d(id s, SEL c) { note(s, c); return 0.0; }
static struct ret16 stub_16(id s, SEL c) { struct ret16 r = { { 0 } }; note(s, c); return r; }
static struct ret24 stub_24(id s, SEL c) { struct ret24 r = { { 0 } }; note(s, c); return r; }
static struct ret32 stub_32(id s, SEL c) { struct ret32 r = { { 0 } }; note(s, c); return r; }
static struct ret48 stub_48(id s, SEL c) { struct ret48 r = { { 0 } }; note(s, c); return r; }
static struct ret64 stub_64(id s, SEL c) { struct ret64 r = { { 0 } }; note(s, c); return r; }

/* The stub whose return value travels the way this method's does, or NULL if none fits. */
static IMP
stub_for(const char *types)
{
	const char *t = types;
	NSUInteger size = 0, align = 0;

	while (*t != '\0' && strchr("rnNoORV", *t) != NULL) {
		t++;
	}
	switch (*t) {
	case 'v':
		return (IMP)stub_v;
	case 'f':
		return (IMP)stub_f;
	case 'd':
		return (IMP)stub_d;
	case '{':
	case '(':
	case '[':
		NSGetSizeAndAlignment(t, &size, &align);
		switch (size) {
		case 1: case 2: case 4: case 8: case 16:
			return (IMP)stub_16;   /* returned in registers; our width is harmless */
		case 24:
			return (IMP)stub_24;
		case 32:
			return (IMP)stub_32;
		case 40: case 48:
			return (IMP)stub_48;
		case 56: case 64:
			return (IMP)stub_64;
		default:
			return NULL;           /* a width we would write past: say so instead */
		}
	default:
		return (IMP)stub_i;        /* integers, pointers, objects, classes, selectors */
	}
}

/* Required instance methods of a protocol and of everything it adopts. */
static unsigned
stub_protocol(Class sub, Protocol *proto, int depth)
{
	unsigned installed = 0, n = 0;

	if (proto == NULL || depth > 4) {
		return 0;
	}
	struct objc_method_description *req =
	    protocol_copyMethodDescriptionList(proto, YES, YES, &n);
	printf("stubdev: protocol %s requires %u instance methods\n", protocol_getName(proto), n);
	for (unsigned i = 0; i < n; i++) {
		SEL sel = req[i].name;
		const char *types = req[i].types;

		if (class_getInstanceMethod(sub, sel) != NULL) {
			continue;   /* the chain already answers it: leave Apple's code alone */
		}
		IMP imp = types != NULL ? stub_for(types) : NULL;
		if (imp == NULL) {
			printf("stubdev:   NOT stubbed: %-44s returns %s\n", sel_getName(sel),
			       types != NULL ? types : "?");
			continue;
		}
		if (class_addMethod(sub, sel, imp, types)) {
			/* the name, because this list IS the work order for a driver on this
			 * class: every one of them is answered by nothing in the chain */
			printf("stubdev:   owed by a subclass: %-52s %s\n", sel_getName(sel), types);
			installed++;
		}
	}
	free(req);

	unsigned on = 0;
	struct objc_method_description *opt =
	    protocol_copyMethodDescriptionList(proto, NO, YES, &on);
	free(opt);
	printf("stubdev: protocol %s has %u optional instance methods, left alone on purpose\n",
	       protocol_getName(proto), on);

	unsigned pn = 0;
	Protocol * __unsafe_unretained *sub_protos = protocol_copyProtocolList(proto, &pn);
	for (unsigned i = 0; i < pn; i++) {
		installed += stub_protocol(sub, sub_protos[i], depth + 1);
	}
	free(sub_protos);
	return installed;
}

/* MTLAddDevice is in Metal's symbol table but not in its export trie, so it cannot be
 * linked against; it is reached the same way every other address here is, by the slide. */
#define MTL_ADD_DEVICE_UNSLID 0x181a1875cUL

/* What -featureProfile answers, per class. The first version of this was a single global,
 * and the object handed to MTLAddDevice then answered with whatever the sweep had left
 * behind -- a value that aborts inside initLimits. A device's feature profile belongs to
 * its class, so it is kept that way. */
static struct { Class cls; long profile; } profiles[16];
static unsigned nprofiles;

static long
profile_of(Class c)
{
	for (unsigned i = 0; i < nprofiles; i++) {
		if (profiles[i].cls == c) {
			return profiles[i].profile;
		}
	}
	return 0;
}

static id
init_by_init(id self, SEL cmd, io_service_t port)
{
	(void)port;
	note(self, cmd);
	/* A driver's own init would open its kernel interface here. The question this mode
	 * asks is what Metal demands, so it does the least that can succeed: _MTLDevice's own
	 * -init, which exists (its method list has it) and sets up nothing device-specific. */
	return ((id (*)(id, SEL))objc_msgSend)(self, sel_getUid("init"));
}

static long
stub_profile(id self, SEL cmd)
{
	note(self, cmd);
	return profile_of(object_getClass(self));
}

static unsigned long
stub_maxbuflen(id self, SEL cmd)
{
	note(self, cmd);
	return 256UL << 20;   /* a plausible answer; nothing here depends on the value */
}

/* The real encoding if anything in the chain or in the protocols has one, else a guess. */
static const char *
encoding_for(Class base, SEL sel, const char *fallback)
{
	Method m = class_getInstanceMethod(base, sel);

	if (m != NULL && method_getTypeEncoding(m) != NULL) {
		return method_getTypeEncoding(m);
	}
	const char *names[] = { "MTLDeviceSPI", "MTLDevice" };
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		Protocol *p = objc_getProtocol(names[i]);

		if (p == NULL) {
			continue;
		}
		for (int req = 1; req >= 0; req--) {
			struct objc_method_description d =
			    protocol_getMethodDescription(p, sel, req != 0, YES);

			if (d.types != NULL) {
				return d.types;
			}
		}
	}
	return fallback;
}

/* What the contract has been found to need beyond the first three, one per refusal. */
static SEL extra_sels[48];
static unsigned nextra;

/* "-[Class selector]: unrecognized selector sent to instance 0x..." names the next method
 * the contract needs. Remembering it and building a new class with it turns one refusal
 * per boot into one boot: the log then lists the whole contract, in the order Metal asks.
 * Returns 0 when the reason is something else, or names a selector already remembered --
 * which is the signal that the loop is not making progress and must stop. */
static int
remember_missing(NSException *e)
{
	const char *reason = [[e reason] UTF8String];
	char name[160];

	if (reason == NULL || strstr(reason, "unrecognized selector") == NULL) {
		return 0;
	}
	const char *space = strchr(reason, ' ');
	const char *close = space != NULL ? strchr(space, ']') : NULL;

	if (space == NULL || close == NULL || (size_t)(close - space - 1) >= sizeof(name)) {
		return 0;
	}
	size_t n = (size_t)(close - space - 1);
	memcpy(name, space + 1, n);
	name[n] = '\0';

	SEL sel = sel_getUid(name);
	for (unsigned i = 0; i < nextra; i++) {
		if (extra_sels[i] == sel) {
			return 0;
		}
	}
	if (nextra >= sizeof(extra_sels) / sizeof(extra_sels[0])) {
		return 0;
	}
	extra_sels[nextra++] = sel;
	printf("stubdev: the contract also wants -%s (%u so far); again with it stubbed\n",
	       name, nextra);
	fflush(stdout);
	return 1;
}

/* One subclass with the three methods MTLAddDevice first asked for, plus whatever later
 * refusals have named, and nothing else. */
static Class
minimal_subclass(Class base, long profile, unsigned attempt)
{
	char name[64];

	snprintf(name, sizeof(name), "RedwoodMinimalDevice%ld_%u", profile, attempt);
	Class sub = objc_allocateClassPair(base, name, 0);
	if (sub == Nil) {
		printf("stubdev: objc_allocateClassPair(%s) failed\n", name);
		return Nil;
	}
	const char *protos[] = { "MTLDeviceSPI", "MTLDevice" };
	for (size_t i = 0; i < sizeof(protos) / sizeof(protos[0]); i++) {
		Protocol *p = objc_getProtocol(protos[i]);

		if (p == NULL) {
			printf("stubdev: protocol %s is not in this process\n", protos[i]);
		} else if (!class_addProtocol(sub, p) && ![sub conformsToProtocol:p]) {
			printf("stubdev: could not declare %s\n", protos[i]);
		}
		/* MTLDeviceSPI adopts MTLDevice, so the second call is refused as redundant and
		 * the class conforms to both; that is not a failure and is not reported as one. */
	}
	if (nprofiles < sizeof(profiles) / sizeof(profiles[0])) {
		profiles[nprofiles].cls = sub;
		profiles[nprofiles].profile = profile;
		nprofiles++;
	}
	SEL isel = sel_getUid("initWithAcceleratorPort:");
	SEL psel = sel_getUid("featureProfile");
	SEL bsel = sel_getUid("maxBufferLength");
	class_addMethod(sub, isel, (IMP)init_by_init, encoding_for(base, isel, "@20@0:8I16"));
	class_addMethod(sub, psel, (IMP)stub_profile, encoding_for(base, psel, "q16@0:8"));
	class_addMethod(sub, bsel, (IMP)stub_maxbuflen, encoding_for(base, bsel, "Q16@0:8"));
	for (unsigned i = 0; i < nextra; i++) {
		const char *types = encoding_for(base, extra_sels[i], "q16@0:8");
		IMP imp = stub_for(types);

		if (imp == NULL) {
			printf("stubdev:   cannot stub -%s: it returns %s, a width this program "
			       "has no stub for\n", sel_getName(extra_sels[i]), types);
			continue;
		}
		class_addMethod(sub, extra_sels[i], imp, types);
	}
	objc_registerClassPair(sub);
	return sub;
}

/* Every field of _MTLDevice's _limits, by name, read out of the object.
 *
 * This is the table -initLimits fills from -featureProfile, and it is the clearest
 * statement of what a driver has to answer for: 64 fields, named by Apple. The names, the
 * types and the offset of the struct all come from the Objective-C runtime in this
 * process, so nothing about Apple's layout is written down here -- the field offsets are
 * computed by walking the type encoding the runtime hands over. */
static void
dump_limits(id obj, Class base)
{
	Ivar iv = class_getInstanceVariable(base, "_limits");

	if (iv == NULL) {
		printf("stubdev:   no _limits ivar in this build\n");
		return;
	}
	const char *e = ivar_getTypeEncoding(iv);
	const unsigned char *p = (const unsigned char *)obj + ivar_getOffset(iv);
	size_t off = 0, printed = 0;

	if (e == NULL) {
		printf("stubdev:   _limits has no type encoding\n");
		return;
	}
	while (*e != '\0' && *e != '=') {   /* skip "{?=" */
		e++;
	}
	if (*e == '=') {
		e++;
	}
	while (*e != '\0' && *e != '}') {
		if (*e != '"') {
			e++;
			continue;
		}
		const char *ns = ++e;

		while (*e != '\0' && *e != '"') {
			e++;
		}
		int nlen = (int)(e - ns);
		if (*e == '"') {
			e++;
		}
		char t = *e++;
		size_t sz = (t == 'Q' || t == 'q' || t == 'd') ? 8 : 4;

		off = (off + sz - 1) & ~(sz - 1);
		if (t == 'f') {
			float v;

			memcpy(&v, p + off, sizeof(v));
			printf("stubdev:     %-44.*s %g\n", nlen, ns, (double)v);
		} else if (sz == 8) {
			unsigned long long v;

			memcpy(&v, p + off, sizeof(v));
			printf("stubdev:     %-44.*s %llu\n", nlen, ns, v);
		} else {
			unsigned int v;

			memcpy(&v, p + off, sizeof(v));
			printf("stubdev:     %-44.*s %u\n", nlen, ns, v);
		}
		off += sz;
		printed++;
	}
	printf("stubdev:   %zu fields of _limits, %zu bytes accounted for\n", printed, off);
	fflush(stdout);
}

/* MTLAddDevice's sequence, by hand, so that a failure is attributable to a step. */
static id
replay_registration(Class base, long profile, io_service_t svc, unsigned attempt)
{
	Class sub = minimal_subclass(base, profile, attempt);

	if (sub == Nil) {
		return nil;
	}
	printf("stubdev: -- featureProfile = %ld --\n", profile);
	fflush(stdout);

	if (sigsetjmp(recover, 1) != 0) {
		printf("stubdev:   profile %ld: the fault above is its answer\n", profile);
		return nil;
	}
	guarded = 1;

	id obj = nil;
	@try {
		obj = ((id (*)(id, SEL, io_service_t))objc_msgSend)(
		    [sub alloc], sel_getUid("initWithAcceleratorPort:"), svc);
		if (obj == nil) {
			printf("stubdev:   step 1 initWithAcceleratorPort: gave nil\n");
			guarded = 0;
			return nil;
		}
		Protocol *dev = objc_getProtocol("MTLDevice");
		Protocol *spi = objc_getProtocol("MTLDeviceSPI");
		printf("stubdev:   step 2 conformsToProtocol(MTLDevice)    %s\n",
		       dev != NULL && [obj conformsToProtocol:dev] ? "yes" : "NO -- fatal there");
		printf("stubdev:   step 3 conformsToProtocol(MTLDeviceSPI) %s\n",
		       spi != NULL && [obj conformsToProtocol:spi] ? "yes" : "NO -- fatal there");
		fflush(stdout);
		((void (*)(id, SEL))objc_msgSend)(obj, sel_getUid("initLimits"));
		printf("stubdev:   step 4 initLimits returned\n");
		fflush(stdout);
		((void (*)(id, SEL))objc_msgSend)(obj, sel_getUid("initFeatureQueries"));
		printf("stubdev:   step 5 initFeatureQueries returned\n");
		fflush(stdout);
	} @catch (NSException *e) {
		printf("stubdev:   profile %ld threw %s: %s\n", profile, [[e name] UTF8String],
		       [[e reason] UTF8String]);
		guarded = 0;
		(void)remember_missing(e);
		return nil;
	}
	guarded = 0;
	printf("stubdev:   profile %ld SURVIVED the registration sequence\n", profile);
	fflush(stdout);
	return obj;
}

/* What such an object can actually answer, asked one question at a time. */
static void
interrogate(id obj)
{
	if (sigsetjmp(recover, 1) != 0) {
		printf("stubdev:   the fault above ended the questions\n");
		return;
	}
	guarded = 1;
	@try {
		printf("stubdev:   name = \"%s\"\n",
		       [[(id<MTLDevice>)obj name] UTF8String]);
	} @catch (NSException *e) {
		printf("stubdev:   name threw %s\n", [[e name] UTF8String]);
	}
	@try {
		printf("stubdev:   supportsFamily(MTLGPUFamilyApple1) = %s\n",
		       [(id<MTLDevice>)obj supportsFamily:MTLGPUFamilyApple1] ? "yes" : "no");
	} @catch (NSException *e) {
		printf("stubdev:   supportsFamily: threw %s\n", [[e name] UTF8String]);
	}
	@try {
		MTLSize t = [(id<MTLDevice>)obj maxThreadsPerThreadgroup];

		printf("stubdev:   maxThreadsPerThreadgroup = %zu x %zu x %zu\n", t.width,
		       t.height, t.depth);
	} @catch (NSException *e) {
		printf("stubdev:   maxThreadsPerThreadgroup threw %s\n", [[e name] UTF8String]);
	}
	@try {
		/* expected to refuse: no entry for it anywhere in the chain. The refusal is the
		 * first item of the next piece of work, so it is asked for on purpose. */
		id buf = [(id<MTLDevice>)obj newBufferWithLength:4096
							 options:MTLResourceStorageModeShared];
		printf("stubdev:   newBufferWithLength:options: gave %s\n",
		       buf != nil ? class_getName(object_getClass(buf)) : "nil");
	} @catch (NSException *e) {
		printf("stubdev:   newBufferWithLength:options: threw %s: %s\n",
		       [[e name] UTF8String], [[e reason] UTF8String]);
	}
	guarded = 0;
	fflush(stdout);
}

/* Metal's own AIR container parser, exercised through a device of ours.
 *
 * The reversing of the library path says newLibraryWithData: asks the driver nothing: it
 * runs MTLLibraryBuilder inside Metal.framework and hands back Metal's own _MTLLibrary,
 * and newFunctionWithName: hands back Metal's own _MTLFunctionInternal. If that holds here,
 * then the container reading and the function objects are free for this port, and the AIR
 * compiler written for subsystem (1) is wanted one level further in, at pipeline creation.
 * A nil library would say the opposite just as clearly.
 *
 * The file is one of the system's own metallibs, read where it already lies on the device;
 * nothing is shipped for this. */
static void
library_probe(id dev, const char *path)
{
	NSError *err = nil;
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];

	if (data == nil) {
		printf("stubdev:   cannot read %s\n", path);
		return;
	}
	printf("stubdev:   %s is %zu bytes\n", path, (size_t)[data length]);
	fflush(stdout);

	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> lib = nil;
	@try {
		lib = [(id<MTLDevice>)dev newLibraryWithData:dd error:&err];
	} @catch (NSException *e) {
		printf("stubdev:   newLibraryWithData: threw %s: %s\n", [[e name] UTF8String],
		       [[e reason] UTF8String]);
		return;
	}
	if (lib == nil) {
		printf("stubdev:   newLibraryWithData: gave nil, error: %s\n",
		       err != nil ? [[err description] UTF8String] : "(none)");
		return;
	}
	printf("stubdev:   newLibraryWithData: gave a %s\n", class_getName(object_getClass(lib)));
	NSArray<NSString *> *names = [lib functionNames];
	printf("stubdev:   it names %lu functions%s\n", (unsigned long)[names count],
	       [names count] > 0 ? ", the first few:" : "");
	for (NSUInteger i = 0; i < [names count] && i < 4; i++) {
		printf("stubdev:     %s\n", [names[i] UTF8String]);
	}
	fflush(stdout);
	if ([names count] == 0) {
		return;
	}
	id<MTLFunction> fn = [lib newFunctionWithName:names[0]];
	printf("stubdev:   newFunctionWithName: gave %s\n",
	       fn != nil ? class_getName(object_getClass(fn)) : "nil");
	if (fn == nil) {
		return;
	}
	/* And the wall: the pipeline entry point goes through -[_MTLDevice compiler], which is
	 * `mov x0, #0; ret`. A driver of ours overrides the entry point instead, which is
	 * where the AIR frontend belongs. The nil here is the measurement that says so. */
	err = nil;
	@try {
		id ps = [(id<MTLDevice>)dev newComputePipelineStateWithFunction:fn error:&err];

		printf("stubdev:   newComputePipelineStateWithFunction: gave %s, error: %s\n",
		       ps != nil ? class_getName(object_getClass(ps)) : "nil",
		       err != nil ? [[err description] UTF8String] : "(none)");
	} @catch (NSException *e) {
		printf("stubdev:   newComputePipelineStateWithFunction: threw %s: %s\n",
		       [[e name] UTF8String], [[e reason] UTF8String]);
	}
	fflush(stdout);
}

/* The one kernel call the IOGPU road needs before anything else, made directly.
 *
 * -[IOGPUMetalDevice initWithAcceleratorPort:] returned nil here without asking our class
 * a single question, which puts the refusal inside IOGPUDeviceCreateWithAPIProperty. That
 * function opens the service with type 1 and then calls external methods 2 and 0. Those
 * three calls are made here by hand so that the failure has a number instead of a guess. */
static void
iogpu_gate(io_service_t svc)
{
	io_connect_t conn = IO_OBJECT_NULL;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 1, &conn);

	printf("stubdev: IOServiceOpen(sgx, type 1) = %#x%s\n", kr,
	       kr == KERN_SUCCESS ? "" : " -- this is what stopped IOGPU's init");
	if (kr != KERN_SUCCESS) {
		return;
	}
	unsigned char out2[0x218] = { 0 };
	size_t n2 = sizeof(out2);

	kr = IOConnectCallStructMethod(conn, 2, NULL, 0, out2, &n2);
	printf("stubdev: external method 2 (device create blob, %zu bytes out) = %#x\n",
	       sizeof(out2), kr);

	unsigned char out0[0x40] = { 0 };
	size_t n0 = sizeof(out0);

	kr = IOConnectCallStructMethod(conn, 0, NULL, 0, out0, &n0);
	printf("stubdev: external method 0 (device config, %zu bytes out) = %#x\n",
	       sizeof(out0), kr);
	IOServiceClose(conn);
	fflush(stdout);
}

static int
minimal_mode(const char *base_name, io_service_t svc)
{
	Class base = objc_getClass(base_name);

	if (base == Nil) {
		printf("stubdev: %s is not in this process\n", base_name);
		return 1;
	}
	/* Metal's own answer first, which also runs the once-block that creates the device
	 * array; MTLAddDevice below appends to that array, so the order matters. */
	printf("stubdev: before anything, MTLCreateSystemDefaultDevice() = %s\n",
	       MTLCreateSystemDefaultDevice() != nil ? "an object" : "nil");
	fflush(stdout);

	long survivor_profile = -1;
	for (long profile = 0; profile <= 12; profile++) {
		if (replay_registration(base, profile, svc, 0) != nil && survivor_profile < 0) {
			survivor_profile = profile;
		}
	}
	if (survivor_profile < 0) {
		printf("stubdev: no featureProfile value survived, so this contract is not "
		       "enough as it stands\n");
		return 0;
	}

	/* Now Metal's own registration, retried: each refusal names one more method the
	 * contract needs, and the next attempt has it. The object is rebuilt each time so
	 * that nothing half-initialised is handed on. */
	id survivor = nil, dev = nil;
	for (unsigned attempt = 1; attempt <= 32 && dev == nil; attempt++) {
		survivor = replay_registration(base, survivor_profile, svc, attempt);
		if (survivor == nil) {
			printf("stubdev: attempt %u: the sequence itself refused; stopping\n",
			       attempt);
			break;
		}
		void (*add_device)(id) = (void (*)(id))(cache_slide() + MTL_ADD_DEVICE_UNSLID);

		if (sigsetjmp(recover, 1) != 0) {
			printf("stubdev: attempt %u: MTLAddDevice faulted\n", attempt);
			break;
		}
		guarded = 1;
		@try {
			add_device(survivor);
			guarded = 0;
			printf("stubdev: attempt %u: MTLAddDevice returned\n", attempt);
			dev = MTLCreateSystemDefaultDevice();
		} @catch (NSException *e) {
			guarded = 0;
			printf("stubdev: attempt %u: MTLAddDevice threw %s: %s\n", attempt,
			       [[e name] UTF8String], [[e reason] UTF8String]);
			if (!remember_missing(e)) {
				printf("stubdev: nothing more can be learnt from that refusal; "
				       "stopping\n");
				break;
			}
		}
		fflush(stdout);
	}
	printf("stubdev: the contract, as Metal asked for it: -initWithAcceleratorPort:, "
	       "-featureProfile, -maxBufferLength");
	for (unsigned i = 0; i < nextra; i++) {
		printf(", -%s", sel_getName(extra_sels[i]));
	}
	printf("\n");
	printf("stubdev: MTLCreateSystemDefaultDevice() = %s\n",
	       dev != nil ? class_getName(object_getClass(dev)) : "nil");
	if (dev != nil) {
		printf("stubdev: so this port can HOLD a Metal device object. It still cannot be "
		       "FOUND: discovery needs the two service properties and a bundle, which "
		       "this run did not touch.\n");
	}
	fflush(stdout);
	if (survivor == nil) {
		iogpu_gate(svc);
		return 0;
	}
	printf("stubdev: the limits table Apple's initLimits filled for featureProfile = %ld:\n",
	       survivor_profile);
	dump_limits(survivor, base);
	printf("stubdev: asking that object what it knows\n");
	interrogate(survivor);

	printf("stubdev: what Metal itself can do with such a device:\n");
	library_probe(dev != nil ? dev : survivor,
		      "/System/Library/VideoDecoders/AV1SW.metallib");
	iogpu_gate(svc);
	return 0;
}

int
main(int argc, char **argv)
{
	struct sigaction sa = { 0 };
	const char *base_name = "IOGPUMetalDevice";
	char sub_name[128];

	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = fault;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGTRAP, &sa, NULL);
	/* Apple's assertion ends in abort(), which is how IOGPUMetalDevice refused last time;
	 * without this the refusal is the end of the program rather than a reading. */
	sigaction(SIGABRT, &sa, NULL);

	int minimal = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--minimal") == 0) {
			minimal = 1;
		} else {
			base_name = argv[i];
		}
	}

	void *iogpu = dlopen("/System/Library/PrivateFrameworks/IOGPU.framework/IOGPU", RTLD_NOW);
	printf("stubdev: dlopen(IOGPU.framework) %s\n", iogpu != NULL ? "loaded" : dlerror());

	if (minimal) {
		io_iterator_t mit = IO_OBJECT_NULL;
		CFMutableDictionaryRef mm = IOServiceMatching("IOAcceleratorES");
		io_service_t ms = IO_OBJECT_NULL;

		if (mm != NULL &&
		    IOServiceGetMatchingServices(kIOMainPortDefault, mm, &mit) == KERN_SUCCESS) {
			ms = IOIteratorNext(mit);
		}
		printf("stubdev: minimal mode on %s, service port %u\n", base_name, (unsigned)ms);
		return minimal_mode(base_name, ms);
	}

	Class base = objc_getClass(base_name);
	if (base == Nil) {
		printf("stubdev: %s is not in this process\n", base_name);
		return 1;
	}
	snprintf(sub_name, sizeof(sub_name), "Redwood%sStub", base_name);
	Class sub = objc_allocateClassPair(base, sub_name, 0);
	if (sub == Nil) {
		printf("stubdev: objc_allocateClassPair(%s) failed\n", sub_name);
		return 1;
	}
	printf("stubdev: %s : %s, instance size %zu\n", sub_name, base_name,
	       class_getInstanceSize(sub));

	/* the gate the assertion reads, and the protocol it means */
	unsigned installed = 0;
	const char *protos[] = { "MTLDeviceSPI", "MTLDevice" };
	for (size_t i = 0; i < sizeof(protos) / sizeof(protos[0]); i++) {
		Protocol *p = objc_getProtocol(protos[i]);

		if (p == NULL) {
			printf("stubdev: protocol %s is not in this process\n", protos[i]);
			continue;
		}
		if (!class_addProtocol(sub, p) && ![sub conformsToProtocol:p]) {
			printf("stubdev: class_addProtocol(%s) refused\n", protos[i]);
			continue;
		}
		/* MTLDeviceSPI adopts MTLDevice, so the second call is refused as redundant
		 * while the class conforms to both; that is not a failure. */
		installed += stub_protocol(sub, p, 0);
	}
	objc_registerClassPair(sub);
	printf("stubdev: %u stubs installed; conformsToProtocol(MTLDeviceSPI) = %s\n", installed,
	       objc_getProtocol("MTLDeviceSPI") != NULL &&
	       [sub conformsToProtocol:objc_getProtocol("MTLDeviceSPI")] ? "yes" : "no");

	io_iterator_t it = IO_OBJECT_NULL;
	CFMutableDictionaryRef match = IOServiceMatching("IOAcceleratorES");
	if (match == NULL ||
	    IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) {
		printf("stubdev: no IOAcceleratorES service\n");
		return 1;
	}
	io_service_t svc = IOIteratorNext(it);
	if (svc == IO_OBJECT_NULL) {
		printf("stubdev: the IOAcceleratorES iterator is empty\n");
		return 1;
	}
	io_name_t sname = { 0 };
	IORegistryEntryGetName(svc, sname);
	printf("stubdev: step 5 against service \"%s\" (port %u); every question follows\n",
	       sname, (unsigned)svc);
	fflush(stdout);

	if (sigsetjmp(recover, 1) != 0) {
		printf("stubdev: the fault above ended it after %u questions\n", asked);
		return 0;
	}
	guarded = 1;
	id obj = nil;
	@try {
		obj = ((id (*)(id, SEL, io_service_t))objc_msgSend)(
		    [sub alloc], sel_getUid("initWithAcceleratorPort:"), svc);
	} @catch (NSException *e) {
		printf("stubdev: step 5 threw %s: %s\n", [[e name] UTF8String],
		       [[e reason] UTF8String]);
		obj = nil;
	}
	guarded = 0;
	printf("stubdev: step 5 %s after %u questions\n",
	       obj != nil ? "RETURNED AN OBJECT" : "gave nothing", asked);
	fflush(stdout);
	return 0;
}
