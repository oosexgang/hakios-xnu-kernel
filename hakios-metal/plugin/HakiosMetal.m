/*
 * HakiosMetal: the plug-in Metal loads when the kernel names it (boot-arg metal=1), STEP ONE: a device that exists.
 *
 * What Metal does (docs/ru/metal.md section 1): it finds the IOKit service of class IOAcceleratorES, reads its
 * properties MetalPluginName ("HakiosMetal") and MetalPluginClassName ("HakiosMetalDevice"), opens
 * /System/Library/Extensions/HakiosMetal.bundle, takes that class, insists that it is a subclass of Metal's private
 * _MTLDevice, and sends it -initWithAcceleratorPort:. That object is the system's one device.
 *
 * What this is, and is not. It is the first link of that chain and nothing more: HakiosMetalDevice is a subclass of
 * _MTLDevice that answers -initWithAcceleratorPort: and -name. It cannot make a buffer, a library, a pipeline or a command
 * queue yet, and says so: a message it does not know is logged ("HakiosMetal: not implemented: -newBufferWithLength:...")
 * and answers nil/0/NO instead of crashing, so that the question "what does iOS ask a device first, and in what order?" can
 * be read off the log rather than guessed. The compiler for the shaders is a separate piece and is largely done
 * (docs/air.md: Apple's shader bitcode into Mesa's NIR); the command queue and the kernel side are not started (docs/metal-plan.md).
 *
 * _MTLDevice is not in the SDK's Metal.tbd (it lives in the system's own copy of the framework), so the class is made at run
 * time, in the constructor that runs when the bundle is loaded, by objc_allocateClassPair on the _MTLDevice that is there.
 * If there is none, nothing is registered, and Metal drops this plug-in as it does any that is not a _MTLDevice.
 *
 * Status: written without having been run. Whether _MTLDevice's own -init is safe to call, what Metal asks of the device right
 * after it is made, and whether -name is enough for MTLCreateSystemDefaultDevice() to hand it out, are what the first run (QEMU,
 * userland/rc.jobs.metal with metal=1) is for.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

static Class gBase;                     /* Metal's private _MTLDevice */
static unsigned int gPort;              /* the IOAcceleratorES service this device was made for */

static void
say(NSString * text)
{
	NSLog(@"HakiosMetal: %@", text);
}

/* -initWithAcceleratorPort: the fifth step of how Metal finds a device. */
static id
deviceInit(id self, SEL _cmd, unsigned int port)
{
	struct objc_super sup = { self, gBase };
	id (*send)(struct objc_super *, SEL) = (id (*)(struct objc_super *, SEL))objc_msgSendSuper;

	self = send(&sup, @selector(init));
	gPort = port;
	say([NSString stringWithFormat:@"a device for the accelerator port %u (%@)", port, self ? @"made" : @"_MTLDevice's init gave nil"]);
	return self;
}

static id
deviceName(__unused id self, __unused SEL _cmd)
{
	return @"HakiOS Adreno 642L (Metal: a device that exists, nothing more)";
}

static uint64_t
deviceRegistryID(__unused id self, __unused SEL _cmd)
{
	return gPort;
}

/* the protocols the class declares (the ones Metal checks for), for the signatures below */
static Protocol * gProtocols[4];
static unsigned gProtocolCount;

/*
 * A message the class does not have, but a protocol it declares does: Metal sends the whole of MTLDevice and the private
 * MTLDeviceSPI, and a method that is missing would end the process ("unrecognized selector": -featureProfile was the first).
 * The type of the answer is read from the protocol's own description of the method, so that zero comes back in the right
 * size, and the message is logged once. What a method should answer is learned one message at a time from this log.
 */
static id
deviceSignature(id self, SEL _cmd, SEL selector)
{
	for (unsigned i = 0; i < gProtocolCount; i++) {
		for (int required = 1; required >= 0; required--) {
			struct objc_method_description d = protocol_getMethodDescription(gProtocols[i], selector, required, YES);

			if (d.name != NULL && d.types != NULL) {
				id (*make)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))objc_msgSend;

				return make(objc_getClass("NSMethodSignature"), sel_registerName("signatureWithObjCTypes:"), d.types);
			}
		}
	}
	struct objc_super sup = { self, gBase };
	id (*send)(struct objc_super *, SEL, SEL) = (id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper;

	(void)_cmd;
	return send(&sup, sel_registerName("methodSignatureForSelector:"), selector);
}

static void
deviceForward(id self, SEL _cmd, id invocation)
{
	static NSMutableSet * seen;
	SEL selector = ((SEL (*)(id, SEL))objc_msgSend)(invocation, sel_registerName("selector"));
	id signature = ((id (*)(id, SEL))objc_msgSend)(invocation, sel_registerName("methodSignature"));
	NSUInteger length = ((NSUInteger (*)(id, SEL))objc_msgSend)(signature, sel_registerName("methodReturnLength"));
	NSString * name = NSStringFromSelector(selector);

	(void)self;
	(void)_cmd;
	@synchronized(gBase) {
		if (seen == nil) {
			seen = [NSMutableSet new];
		}
		if (![seen containsObject:name]) {
			[seen addObject:name];
			say([NSString stringWithFormat:@"asked and not implemented: -%@ (returns %lu bytes; answered with zero)", name, (unsigned long)length]);
		}
	}
	if (length != 0) {
		void * zero = calloc(1, length);

		((void (*)(id, SEL, void *))objc_msgSend)(invocation, sel_registerName("setReturnValue:"), zero);
		free(zero);
	}
}

__attribute__((constructor)) static void
registerDevice(void)
{
	Class device;

	gBase = objc_getClass("_MTLDevice");
	if (gBase == Nil) {
		say(@"this system has no _MTLDevice: no device class is registered");
		return;
	}
	if (objc_getClass("HakiosMetalDevice") != Nil) {
		return;
	}
	device = objc_allocateClassPair(gBase, "HakiosMetalDevice", 0);
	if (device == Nil) {
		say(@"could not make the class HakiosMetalDevice");
		return;
	}
	/*
	 * The first run said it: MTLAddDevice asserts [newDevice conformsToProtocol:@protocol(MTLDevice)] and aborts the process
	 * (backboardd) when the class does not declare it. _MTLDevice itself does not, so the class does.
	 */
	class_addProtocol(device, @protocol(MTLDevice));
	gProtocols[gProtocolCount++] = @protocol(MTLDevice);
	/*
	 * ... and the second run said that MTLAddDevice asserts the private protocol MTLDeviceSPI as well (line 835). Private
	 * protocols are not in the SDK, so they are taken by name from the system's own Metal; each next assertion of this kind
	 * that a run shows is a name for this list.
	 */
	static const char * const privateProtocols[] = { "MTLDeviceSPI", NULL };
	for (const char * const * name = privateProtocols; *name != NULL; name++) {
		Protocol * protocol = objc_getProtocol(*name);

		if (protocol != NULL && gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
			class_addProtocol(device, protocol);
			gProtocols[gProtocolCount++] = protocol;
		} else {
			say([NSString stringWithFormat:@"the protocol %s is not in this system's Metal", *name]);
		}
	}
	class_addMethod(device, @selector(initWithAcceleratorPort:), (IMP)deviceInit, "@@:I");
	class_addMethod(device, @selector(name), (IMP)deviceName, "@@:");
	class_addMethod(device, @selector(registryID), (IMP)deviceRegistryID, "Q@:");
	class_addMethod(device, sel_registerName("methodSignatureForSelector:"), (IMP)deviceSignature, "@@::");
	class_addMethod(device, sel_registerName("forwardInvocation:"), (IMP)deviceForward, "v@:@");
	objc_registerClassPair(device);
	say(@"the class HakiosMetalDevice is registered (a subclass of _MTLDevice)");
}
