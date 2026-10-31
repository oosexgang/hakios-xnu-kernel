/*
 * Subsystem (2), the Metal object graph: which class can a driver of ours inherit?
 *
 * docs/metal.md section 1 has the five steps Metal takes to find a GPU, read out of
 * Metal.framework itself. The last two decide whether a driver of ours is possible:
 *
 *     4. the class named by the bundle must pass [class isSubclassOfClass:_MTLDevice]
 *     5. [[class alloc] initWithAcceleratorPort:service] -- and that object IS the device
 *
 * The first run of this program answered step 4 (a class built at runtime passes) and
 * then died on step 5: "-[RedwoodMetalDevice initWithAcceleratorPort:]: unrecognized
 * selector", although the runtime had just said _MTLDevice implements that selector
 * itself. Both statements were true. The cache says why: the whole body of
 * -[_MTLDevice initWithAcceleratorPort:] (0x181aa8bec) is
 *
 *     mov x2, x1 ; bl _objc_msgSend$doesNotRecognizeSelector: ; mov x0, #0 ; ret
 *
 * which is how Apple writes an abstract method, and doesNotRecognizeSelector: names the
 * RECEIVER's class, so the refusal reads exactly like a missing method. Nothing in the
 * Objective-C runtime can tell such a method from a working one; tools/metal/absmeth.py
 * tells them apart from the cache instead.
 *
 * So the device object is built by a subclass, and the question is which subclass. The
 * chain in this cache is
 *
 *     NSObject -> _MTLDevice (Metal.framework)
 *                   |- MTLIOAccelDevice  (Metal.framework, the legacy IOAccelerator path)
 *                   \- IOGPUMetalDevice  (IOGPU.framework)
 *                        \- AGXA11FamilyDevice <MTLDeviceSPI> (AGXMetalA11.bundle)
 *                             \- AGXA11Device
 *
 * and this program measures all three candidates in one boot, each against this port's
 * own IOAcceleratorES service ("sgx", from iokit/Kernel/MiniIOMobileFramebuffer.cpp):
 *
 *   _MTLDevice        -- the control: its init is the abstract stub above, so a driver of
 *                        ours would write its own and owe every resource selector.
 *   MTLIOAccelDevice  -- Apple's init, over the IOAccelerator user client.
 *   IOGPUMetalDevice  -- Apple's init, over the IOGPU user client: the path the real
 *                        driver takes. Whatever it demands of the kernel, it demands here,
 *                        and the failure names it.
 *
 * None of the three is declared in source. Each subclass is built at RUNTIME with
 * objc_allocateClassPair, which takes the superclass as it is in this process: Apple's ivar
 * layout stays Apple's and there is nothing for us to get wrong. For the same reason the
 * ivars a driver would have to fill are looked up BY NAME at the end -- a driver never has
 * to write an offset down.
 *
 * Each step 5 runs between a sigsetjmp and the fault handler, and inside @try, because the
 * two ways a device class refuses are an Objective-C exception (an abstract method) and a
 * hardware fault (a method that wanted something this port has not set up). The first run
 * had neither guard, so the first refusal killed the process and the two experiments after
 * it went unmeasured.
 *
 * It does NOT give the service the two properties Metal looks for (MetalPluginName,
 * MetalPluginClassName): with them, CoreAnimation and OpenGL ES would both take the Metal
 * path on the next boot, and docs/metal.md section 7 measured that road as fatal while
 * nothing is behind it. This program walks the same steps by hand instead, in its own
 * process, where a crash costs a few lines in a log.
 *
 * Run: /redwood/bin/metal_dev   (RC_JOBS=rc.jobs.metaldev)
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>

/* the one unslid address this program needs, as docs/metal.md section 6 explains the trick */
#define MTL_CREATE_DEVICE_UNSLID 0x181a34b3cUL
extern id MTLCreateSystemDefaultDevice(void);

static sigjmp_buf recover;
static volatile sig_atomic_t guarded;

static uintptr_t
cache_slide(void)
{
	return (uintptr_t)&MTLCreateSystemDefaultDevice - MTL_CREATE_DEVICE_UNSLID;
}

static void
fault(int sig, siginfo_t *info, void *uap)
{
	ucontext_t *uc = uap;
	uintptr_t slide = cache_slide(), pc = 0, lr = 0, fp = 0;

	if (uc != NULL && uc->uc_mcontext != NULL) {
		pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
		lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
		fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
	}
	printf("metal_dev: signal %d at address %p\n", sig, info != NULL ? info->si_addr : NULL);
	printf("metal_dev:   pc %#lx unslid (slide %#lx)\n", pc - slide, slide);
	printf("metal_dev:   lr %#lx unslid\n", lr - slide);
	for (int i = 0; i < 8 && fp > 0x1000; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		printf("metal_dev:   #%d %#lx unslid\n", i, ret - slide);
		if (next <= fp) {
			break;
		}
		fp = next;
	}
	fflush(stdout);
	if (guarded) {
		/* Jump back and keep measuring. The runtime may be left holding a lock, so
		 * everything after this is printed before it is attempted, and a hang here is
		 * itself a reading. */
		guarded = 0;
		siglongjmp(recover, 1);
	}
	_exit(sig);
}

/* every selector a driver is known or suspected to owe, and where that comes from */
static const char *const wanted[] = {
	"initWithAcceleratorPort:",          /* step 5 of the discovery chain */
	"name",                              /* -[MTLDevice name] */
	"registryID",
	"maxThreadsPerThreadgroup",
	"recommendedMaxWorkingSetSize",
	"hasUnifiedMemory",
	"supportsFamily:",
	"supportsFeatureSet:",
	"newCommandQueue",
	"newCommandQueueWithMaxCommandBufferCount:",
	"newBufferWithLength:options:",
	"newBufferWithBytes:length:options:",
	"newTextureWithDescriptor:",
	"newLibraryWithData:error:",
	"newLibraryWithSource:options:error:",
	"newDefaultLibrary",
	"newComputePipelineStateWithFunction:error:",
	"newRenderPipelineStateWithDescriptor:error:",
	"newSamplerStateWithDescriptor:",
	"newDepthStencilStateWithDescriptor:",
	"newHeapWithDescriptor:",
	"newFence",
	"newEvent",
};

static void
report_class(const char *name)
{
	Class c = objc_getClass(name);

	if (c == Nil) {
		printf("metal_dev: class %s: NOT PRESENT in this process\n", name);
		return;
	}
	printf("metal_dev: class %s: instance size %zu, superclass %s\n", name,
	       class_getInstanceSize(c),
	       class_getSuperclass(c) != Nil ? class_getName(class_getSuperclass(c)) : "(none)");

	unsigned n = 0;
	Ivar *ivars = class_copyIvarList(c, &n);
	printf("metal_dev:   %u ivars declared by it:\n", n);
	for (unsigned i = 0; i < n; i++) {
		const char *type = ivar_getTypeEncoding(ivars[i]);
		printf("metal_dev:     +%-5ld %-40s %s\n", ivar_getOffset(ivars[i]),
		       ivar_getName(ivars[i]), type != NULL ? type : "?");
	}
	free(ivars);

	/* An entry in the method list is not behaviour: see the header. The addresses are
	 * printed unslid so that tools/metal/absmeth.py can say which are abstract stubs, and
	 * so that a disagreement with the cache shows up as a different address. */
	printf("metal_dev:   of the selectors a driver owes, %s has, ITSELF or inherited:\n", name);
	for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
		SEL sel = sel_getUid(wanted[i]);
		Method m = class_getInstanceMethod(c, sel);
		unsigned mn = 0;
		Method *own = class_copyMethodList(c, &mn);
		Method mine = NULL;

		for (unsigned k = 0; k < mn; k++) {
			if (method_getName(own[k]) == sel) {
				mine = own[k];
				break;
			}
		}
		free(own);
		if (mine != NULL) {
			printf("metal_dev:     %-46s imp %#lx unslid, its own\n", wanted[i],
			       (uintptr_t)method_getImplementation(mine) - cache_slide());
		} else if (m != NULL) {
			printf("metal_dev:     %-46s imp %#lx unslid, inherited\n", wanted[i],
			       (uintptr_t)method_getImplementation(m) - cache_slide());
		} else {
			printf("metal_dev:     %-46s no entry anywhere in the chain\n", wanted[i]);
		}
	}

	unsigned mn = 0;
	Method *own = class_copyMethodList(c, &mn);
	printf("metal_dev:   %u methods of its own; those naming init or the accelerator:\n", mn);
	for (unsigned i = 0; i < mn; i++) {
		const char *sn = sel_getName(method_getName(own[i]));

		if (strstr(sn, "ccelerator") != NULL || strncmp(sn, "init", 4) == 0 ||
		    strncmp(sn, "_init", 5) == 0) {
			printf("metal_dev:     %-50s imp %#lx unslid\n", sn,
			       (uintptr_t)method_getImplementation(own[i]) - cache_slide());
		}
	}
	free(own);
}

/* Step 5 against one candidate superclass, under both guards. */
static void
try_candidate(const char *base_name, const char *sub_name, io_service_t svc)
{
	Class base = objc_getClass(base_name);
	Class mtldev = objc_getClass("_MTLDevice");

	printf("metal_dev: ---- candidate: a subclass of %s ----\n", base_name);
	if (base == Nil) {
		printf("metal_dev: %s is not in this process, so it cannot be measured here\n",
		       base_name);
		return;
	}
	report_class(base_name);

	Class sub = objc_allocateClassPair(base, sub_name, 0);
	if (sub == Nil) {
		printf("metal_dev: objc_allocateClassPair(%s) failed\n", sub_name);
		return;
	}
	objc_registerClassPair(sub);

	SEL isel = sel_getUid("initWithAcceleratorPort:");
	Method ms = class_getInstanceMethod(sub, isel);
	printf("metal_dev: %s: size %zu, step 4 (isSubclassOfClass:_MTLDevice) %s\n", sub_name,
	       class_getInstanceSize(sub),
	       mtldev != Nil && [sub isSubclassOfClass:mtldev] ? "PASSES" : "fails");
	printf("metal_dev:   it resolves initWithAcceleratorPort: to imp %#lx unslid\n",
	       ms != NULL ? (uintptr_t)method_getImplementation(ms) - cache_slide() : 0UL);
	fflush(stdout);

	if (sigsetjmp(recover, 1) != 0) {
		printf("metal_dev: %s: the fault above is this candidate's answer; carrying on\n",
		       sub_name);
		fflush(stdout);
		return;
	}
	guarded = 1;

	id obj = nil;
	@try {
		obj = ((id (*)(id, SEL, io_service_t))objc_msgSend)([sub alloc], isel, svc);
		printf("metal_dev: %s: initWithAcceleratorPort: returned %s\n", sub_name,
		       obj != nil ? class_getName(object_getClass(obj)) : "nil -- it refused");
	} @catch (NSException *e) {
		printf("metal_dev: %s: initWithAcceleratorPort: threw %s: %s\n", sub_name,
		       [[e name] UTF8String], [[e reason] UTF8String]);
		obj = nil;
	}
	guarded = 0;
	fflush(stdout);

	if (obj == nil) {
		return;
	}
	/* An object came back. The cheapest questions first, each still under the guards. */
	printf("metal_dev: %s: conformsToProtocol(MTLDevice) = %s\n", sub_name,
	       [obj conformsToProtocol:@protocol(MTLDevice)] ? "yes" : "no");
	fflush(stdout);
	if (sigsetjmp(recover, 1) != 0) {
		printf("metal_dev: %s: faulted while being asked its name\n", sub_name);
		return;
	}
	guarded = 1;
	@try {
		printf("metal_dev: %s: name = \"%s\", registryID = %#llx\n", sub_name,
		       [[(id<MTLDevice>)obj name] UTF8String],
		       (unsigned long long)[(id<MTLDevice>)obj registryID]);
	} @catch (NSException *e) {
		printf("metal_dev: %s: asking its name threw %s: %s\n", sub_name,
		       [[e name] UTF8String], [[e reason] UTF8String]);
	}
	guarded = 0;
	fflush(stdout);
}

int
main(void)
{
	struct sigaction sa = { 0 };

	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = fault;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGTRAP, &sa, NULL);

	printf("metal_dev: Metal's own answer first: MTLCreateSystemDefaultDevice() = %s\n",
	       MTLCreateSystemDefaultDevice() != nil ? "an object" : "nil");

	/* IOGPU.framework is not linked by anything in this process, so the third candidate
	 * has to be brought in by hand. Whether it even loads here is part of the answer. */
	const char *iogpu_path = "/System/Library/PrivateFrameworks/IOGPU.framework/IOGPU";
	void *iogpu = dlopen(iogpu_path, RTLD_NOW);
	printf("metal_dev: dlopen(IOGPU.framework) %s\n",
	       iogpu != NULL ? "loaded" : dlerror());

	/* this port's own IOAcceleratorES service, the one Metal would have found */
	io_iterator_t it = IO_OBJECT_NULL;
	CFMutableDictionaryRef match = IOServiceMatching("IOAcceleratorES");
	if (match == NULL ||
	    IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) {
		printf("metal_dev: no IOAcceleratorES service to pass to initWithAcceleratorPort:\n");
		return 1;
	}
	io_service_t svc = IOIteratorNext(it);
	if (svc == IO_OBJECT_NULL) {
		printf("metal_dev: the IOAcceleratorES iterator is empty\n");
		return 1;
	}
	io_name_t sname = { 0 };
	IORegistryEntryGetName(svc, sname);
	printf("metal_dev: the service is \"%s\" (port %u)\n", sname, (unsigned)svc);
	fflush(stdout);

	try_candidate("_MTLDevice", "RedwoodMetalDevice", svc);
	try_candidate("MTLIOAccelDevice", "RedwoodIOAccelDevice", svc);
	try_candidate("IOGPUMetalDevice", "RedwoodIOGPUDevice", svc);

	/* The way out of writing Apple's layout down: a driver asks the runtime where each
	 * ivar is, in the build it is actually running against. */
	static const char *const key_ivars[] = { "_limits", "_architecture",
						 "_compilerConnectionManager", "_libraryBuilder" };
	Class base = objc_getClass("_MTLDevice");
	printf("metal_dev: ivars a driver would fill, found by name rather than by offset:\n");
	for (size_t i = 0; i < sizeof(key_ivars) / sizeof(key_ivars[0]); i++) {
		Ivar iv = base != Nil ? class_getInstanceVariable(base, key_ivars[i]) : NULL;

		if (iv == NULL) {
			printf("metal_dev:   %-28s no such ivar in this build\n", key_ivars[i]);
		} else {
			printf("metal_dev:   %-28s +%ld\n", key_ivars[i], ivar_getOffset(iv));
		}
	}
	fflush(stdout);
	return 0;
}
