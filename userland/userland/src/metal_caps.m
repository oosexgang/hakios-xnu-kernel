/*
 * metal_caps: what this port's Metal device answers when an application asks what it supports.
 *
 * An application decides its whole render path at startup from this family of questions, and
 * this port answers almost none of them itself: -supportsFamily:, -supportsFeatureSet: and
 * some forty more are INHERITED from Apple's private _MTLDevice, which computes them from one
 * number the driver does supply -- -featureProfile, which redwoodmetal.m returns 0 from.
 * Reading Metal's code says what 0 should produce (docs/metal.md 10.48); this program is the
 * other half, because an inherited answer is Apple's code running against ivars nothing in
 * this port fills, and that shape has already cost one crash here (-targetDeviceInfo, 10.40).
 *
 * The device is taken from MTLCreateSystemDefaultDevice() and from nowhere else: that is the
 * road a real application takes, so the ivars behind these answers are filled (or not) by
 * exactly the registration Metal itself ran. It needs the two registry properties on the
 * IOAcceleratorES service, so run `/redwood/bin/metal_props on` first (rc.jobs.metalcaps
 * does). A nil device is reported as such and is not an error of this program's.
 *
 * Every question is sent inside @try. That is not caution: a selector no class in the chain
 * answers raises NSInvalidArgumentException naming OUR class, which is indistinguishable in a
 * log from a method we forgot to write (docs/metal.md 10.1) -- so the exception is caught and
 * printed beside the selector that raised it, and the sweep goes on to the next question
 * instead of taking the whole measurement down with it.
 *
 * Nothing here is cast through a declared protocol: most of these are MTLDeviceSPI, whose
 * header this port has not got, so each send names its own return type at the call. A send
 * made with the wrong return type reads a register that was never written, which is how a
 * "measurement" turns into a plausible number -- hence one helper per return shape and no
 * generic one.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdio.h>

/* MTLGPUFamily and MTLFeatureSet, by name, because the answer is only worth reading next to
 * the name of what was asked. The feature sets are iOS's own block, 0 to 18: Metal's table
 * that maps them to a minimum feature profile has exactly 19 entries (0x181b5bc90), and the
 * last two are named by strings in Metal and not by the 16.5 SDK header. */
static const struct { const char *name; NSUInteger value; } families[] = {
	{ "MTLGPUFamilyApple1", 1001 }, { "MTLGPUFamilyApple2", 1002 },
	{ "MTLGPUFamilyApple3", 1003 }, { "MTLGPUFamilyApple4", 1004 },
	{ "MTLGPUFamilyApple5", 1005 }, { "MTLGPUFamilyApple6", 1006 },
	{ "MTLGPUFamilyApple7", 1007 }, { "MTLGPUFamilyApple8", 1008 },
	{ "MTLGPUFamilyMac1", 2001 },   { "MTLGPUFamilyMac2", 2002 },
	{ "MTLGPUFamilyCommon1", 3001 },{ "MTLGPUFamilyCommon2", 3002 },
	{ "MTLGPUFamilyCommon3", 3003 },
	{ "MTLGPUFamilyMacCatalyst1", 4001 }, { "MTLGPUFamilyMacCatalyst2", 4002 },
	{ "MTLGPUFamilyMetal3", 5001 },
};

static const char *const feature_sets[] = {
	"iOS_GPUFamily1_v1", "iOS_GPUFamily2_v1", "iOS_GPUFamily1_v2", "iOS_GPUFamily2_v2",
	"iOS_GPUFamily3_v1", "iOS_GPUFamily1_v3", "iOS_GPUFamily2_v3", "iOS_GPUFamily3_v2",
	"iOS_GPUFamily1_v4", "iOS_GPUFamily2_v4", "iOS_GPUFamily3_v3", "iOS_GPUFamily4_v1",
	"iOS_GPUFamily1_v5", "iOS_GPUFamily2_v5", "iOS_GPUFamily3_v4", "iOS_GPUFamily4_v2",
	"iOS_GPUFamily5_v1", "iOS_GPUFamily6_v1", "iOS_GPUFamily7_v1",
};

/* one helper per return shape; see the file comment for why there is no generic one */
#define ASK(TYPE, NAME, ARGS, CALL)						\
	static TYPE NAME ARGS							\
	{									\
		SEL sel = sel_getUid(name);					\
		if (![obj respondsToSelector:sel]) {				\
			printf("caps:   %-46s NOT ANSWERED by anything in the chain\n",	\
			       name);						\
			return (TYPE) 0;					\
		}								\
		@try {								\
			return CALL;						\
		} @catch (NSException *e) {					\
			printf("caps:   %-46s raised %s: %s\n", name,		\
			       [[e name] UTF8String], [[e reason] UTF8String]);	\
		}								\
		return (TYPE) 0;						\
	}

ASK(BOOL, ask_bool, (id obj, const char *name),
    ((BOOL (*)(id, SEL))objc_msgSend)(obj, sel))
ASK(uint64_t, ask_uint, (id obj, const char *name),
    ((uint64_t (*)(id, SEL))objc_msgSend)(obj, sel))
ASK(id, ask_obj, (id obj, const char *name),
    ((id (*)(id, SEL))objc_msgSend)(obj, sel))
ASK(BOOL, ask_bool_u, (id obj, const char *name, NSUInteger arg),
    ((BOOL (*)(id, SEL, NSUInteger))objc_msgSend)(obj, sel, arg))
ASK(uint64_t, ask_uint_u, (id obj, const char *name, NSUInteger arg),
    ((uint64_t (*)(id, SEL, NSUInteger))objc_msgSend)(obj, sel, arg))

/* MTLSize comes back in registers on arm64 (three 64-bit words), which objc_msgSend returns
 * as a struct rather than through x8; the cast has to say so or the words are read from the
 * wrong place. Separate from the macro because its zero value is not a scalar. */
static MTLSize
ask_size(id obj, const char *name)
{
	SEL sel = sel_getUid(name);
	MTLSize zero = MTLSizeMake(0, 0, 0);

	if (![obj respondsToSelector:sel]) {
		printf("caps:   %-46s NOT ANSWERED by anything in the chain\n", name);
		return zero;
	}
	@try {
		return ((MTLSize (*)(id, SEL))objc_msgSend)(obj, sel);
	} @catch (NSException *e) {
		printf("caps:   %-46s raised %s\n", name, [[e name] UTF8String]);
	}
	return zero;
}

static void
show_bool(id dev, const char *name)
{
	printf("caps:   %-46s %s\n", name, ask_bool(dev, name) ? "yes" : "no");
}

static void
show_uint(id dev, const char *name)
{
	printf("caps:   %-46s %llu\n", name, (unsigned long long) ask_uint(dev, name));
}

int
main(int argc, char **argv)
{
	(void) argc; (void) argv;
	id dev = MTLCreateSystemDefaultDevice();

	if (dev == nil) {
		printf("caps: MTLCreateSystemDefaultDevice() = nil -- discovery did not reach this "
		       "driver. Needs the kernel booted with metal=1 and `metal_props on` before "
		       "this program (docs/metal.md 1, 10.14); nothing below could be measured.\n");
		return 1;
	}
	printf("caps: device = %s, -name \"%s\"\n", class_getName([dev class]),
	       [[dev name] UTF8String]);

	/* The one number this port supplies, and the input to everything Apple derives. Printed
	 * first because every answer below is a function of it. */
	printf("caps: featureProfile = %llu  (Apple's own A11 driver answers 5:\n"
	       "caps:   -[AGXA11FamilyDevice featureProfile] is `mov w0, #5; ret` at 0x209210f10)\n",
	       (unsigned long long) ask_uint(dev, "featureProfile"));

	printf("caps: -- supportsFamily: over every MTLGPUFamily --\n");
	for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
		printf("caps:   supportsFamily(%-24s = %4lu) %s\n", families[i].name,
		       (unsigned long) families[i].value,
		       ask_bool_u(dev, "supportsFamily:", families[i].value) ? "yes" : "no");
	}

	printf("caps: -- supportsFeatureSet: over iOS's block, and two values outside it --\n");
	for (NSUInteger i = 0; i < sizeof(feature_sets) / sizeof(feature_sets[0]); i++) {
		printf("caps:   supportsFeatureSet(%2lu %-20s) %s\n", (unsigned long) i,
		       feature_sets[i],
		       ask_bool_u(dev, "supportsFeatureSet:", i) ? "yes" : "no");
	}
	/* Outside the table: Metal's `cmp x3, #0x12` sends anything above 18 to a branch that
	 * answers NO without consulting the device, and 30000 is tvOS's block. Both are asked so
	 * that a yes here would be visible rather than assumed away. */
	printf("caps:   supportsFeatureSet(19 out of table      ) %s\n",
	       ask_bool_u(dev, "supportsFeatureSet:", 19) ? "yes" : "no");
	printf("caps:   supportsFeatureSet(30000 tvOS block     ) %s\n",
	       ask_bool_u(dev, "supportsFeatureSet:", 30000) ? "yes" : "no");

	printf("caps: -- the answers Apple DERIVES from the family list --\n");
	/* argumentBuffersSupport is (caps & 7) == 7 over -indirectArgumentBufferCapabilities,
	 * which _MTLDevice answers 0 from (a two-instruction stub at 0x181aac77c), so this
	 * should be 0 = MTLArgumentBuffersTier1 -- and Tier1 is still "supported": the
	 * enumeration has no value for "no argument buffers". */
	printf("caps:   argumentBuffersSupport                     %llu (0 = Tier1, 1 = Tier2)\n",
	       (unsigned long long) ask_uint(dev, "argumentBuffersSupport"));
	/* readWriteTextureSupport asks supportsFamily:(1004) and answers 2 or 0 (0x181a18738) */
	printf("caps:   readWriteTextureSupport                    %llu (0 = TierNone, 2 = Tier2)\n",
	       (unsigned long long) ask_uint(dev, "readWriteTextureSupport"));
	show_bool(dev, "areRasterOrderGroupsSupported");
	show_bool(dev, "areProgrammableSamplePositionsSupported");
	show_bool(dev, "areBarycentricCoordsSupported");

	printf("caps: -- the answers that go through _MTLDevice's _featureQueries ivar --\n");
	/* Each of these is `ldr x0, [self, #0x160]; b objc_msgSend$familySupportsX`. The claim
	 * being tested is that the ivar is nil -- -[MTLDeviceFeatureQueries initWithDevice:] is
	 * `mov x0, #0; ret` at 0x181b21d98 -- so a message to nil returns 0 and every one of
	 * these is NO for every device that does not override them. The last line below asks
	 * that class directly, which settles it on the device rather than in a disassembly. */
	show_bool(dev, "supportsDynamicLibraries");
	show_bool(dev, "supportsRenderDynamicLibraries");
	show_bool(dev, "supportsFunctionPointers");
	show_bool(dev, "supportsFunctionPointersFromRender");
	show_bool(dev, "supports32BitFloatFiltering");
	show_bool(dev, "supports32BitMSAA");
	show_bool(dev, "supportsBCTextureCompression");
	show_bool(dev, "supportsPullModelInterpolation");
	show_bool(dev, "supportsQueryTextureLOD");
	show_bool(dev, "supportsShaderBarycentricCoordinates");
	show_bool(dev, "supportsPrimitiveMotionBlur");
	show_bool(dev, "supportsRaytracing");
	show_bool(dev, "supportsRaytracingFromRender");
	show_bool(dev, "supportsProgrammableBlending");
	show_bool(dev, "supportsMemorylessRenderTargets");
	show_bool(dev, "supportsPlacementHeaps");
	show_bool(dev, "supportsBinaryArchives");
	show_bool(dev, "supportsVertexAmplification");
	show_bool(dev, "supportsTessellation");
	show_bool(dev, "supportsIndirectDrawAndDispatch");

	Class fq = objc_lookUpClass("MTLDeviceFeatureQueries");

	if (fq == Nil) {
		printf("caps:   MTLDeviceFeatureQueries is not in this process at all\n");
	} else {
		id q = nil;

		@try {
			q = ((id (*)(id, SEL, id))objc_msgSend)([fq alloc],
			    sel_getUid("initWithDevice:"), dev);
		} @catch (NSException *e) {
			printf("caps:   [MTLDeviceFeatureQueries initWithDevice:] raised %s\n",
			       [[e name] UTF8String]);
		}
		printf("caps:   [[MTLDeviceFeatureQueries alloc] initWithDevice:dev] = %s\n",
		       q == nil ? "nil -- so every familySupports* above is a message to nil"
				: class_getName([q class]));
	}

	printf("caps: -- counter sampling, which _MTLDevice answers WITHOUT asking the device --\n");
	/* -[_MTLDevice supportsCounterSampling:] (0x181aace20) is eight instructions of
	 * arithmetic on the argument: `(arg-1) < 4 && ((0xb >> ((arg-1) & 0xf)) & 1)`. It reads
	 * no ivar and sends no message, so it answers the same for every device in the system,
	 * including one with no counters at all. The names are MTLCounterSamplingPoint's. */
	static const char *const points[] = { "AtStageBoundary", "AtDrawBoundary",
					      "AtDispatchBoundary", "AtTileDispatchBoundary",
					      "AtBlitBoundary" };
	for (NSUInteger i = 0; i < 5; i++) {
		printf("caps:   supportsCounterSampling(%-22s) %s\n", points[i],
		       ask_bool_u(dev, "supportsCounterSampling:", i) ? "yes" : "no");
	}
	printf("caps:   counterSets                                %s\n",
	       ask_obj(dev, "counterSets") == nil ? "nil" : "a list");

	printf("caps: -- sample counts, where the driver answers for itself --\n");
	/* -supportsTextureSampleCount: is one instruction in _MTLDevice (0x181a17890) that tail
	 * calls -supportsSampleCount:, which _MTLDevice does not implement; redwoodmetal.m does,
	 * and answers count == 1. Both are asked here so that the forward is measured and not
	 * assumed: if they ever disagree, one of them is being answered by something else. */
	for (NSUInteger n = 1; n <= 8; n *= 2) {
		printf("caps:   supportsTextureSampleCount(%lu) %s / supportsSampleCount(%lu) %s\n",
		       (unsigned long) n,
		       ask_bool_u(dev, "supportsTextureSampleCount:", n) ? "yes" : "no ",
		       (unsigned long) n,
		       ask_bool_u(dev, "supportsSampleCount:", n) ? "yes" : "no");
	}

	printf("caps: -- the quantities --\n");
	MTLSize t = ask_size(dev, "maxThreadsPerThreadgroup");

	printf("caps:   maxThreadsPerThreadgroup                   %lu x %lu x %lu\n",
	       (unsigned long) t.width, (unsigned long) t.height, (unsigned long) t.depth);
	show_uint(dev, "maxThreadgroupMemoryLength");
	show_uint(dev, "maxBufferLength");
	show_uint(dev, "recommendedMaxWorkingSetSize");
	show_uint(dev, "currentAllocatedSize");
	show_uint(dev, "maxArgumentBufferSamplerCount");
	show_uint(dev, "maxVertexAmplificationCount");
	show_uint(dev, "maxTessellationFactor");
	show_uint(dev, "registryID");
	show_bool(dev, "hasUnifiedMemory");
	show_bool(dev, "isLowPower");
	show_bool(dev, "isHeadless");
	show_bool(dev, "isRemovable");
	show_bool(dev, "isDepth24Stencil8PixelFormatSupported");
	printf("caps:   vendorName                                 %s\n",
	       [(NSString *) ask_obj(dev, "vendorName") UTF8String]);
	printf("caps:   supportsVertexAmplificationCount(1) %s, (2) %s\n",
	       ask_bool_u(dev, "supportsVertexAmplificationCount:", 1) ? "yes" : "no",
	       ask_bool_u(dev, "supportsVertexAmplificationCount:", 2) ? "yes" : "no");

	/* The alignment an application must honour to make a texture over a buffer. Apple's
	 * answer comes from a per-pixel-format table (_MTLPixelFormatGetInfoForDevice) and not
	 * from the limits table, so it is worth having beside the limits. 70 is
	 * MTLPixelFormatRGBA8Unorm, 115 MTLPixelFormatRGBA32Float, 252 Depth32Float. */
	printf("caps: -- minimumLinearTextureAlignmentForPixelFormat: --\n");
	NSUInteger formats[] = { 70, 115, 252 };

	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		printf("caps:   format %-3lu                                  %llu\n",
		       (unsigned long) formats[i],
		       (unsigned long long) ask_uint_u(dev,
		           "minimumLinearTextureAlignmentForPixelFormat:", formats[i]));
	}
	/*
	 * And the one capability gate the acceptance test actually passes through.
	 *
	 * Resident Evil 4 asks the device almost nothing itself -- of the 1 238 selectors its
	 * metal-cpp table registers, only -hasUnifiedMemory and -currentAllocatedSize have a
	 * send site in its 200 MB of code (tools/metal/appsends.py). Its one real question goes
	 * through MetalFX, which it links and drives as a temporal upscaler: it sends
	 * +[MTLFXTemporalScalerDescriptor supportsDevice:], and THAT asks
	 * -supportsFamily:(MTLGPUFamilyApple7) and -gpuCoreCount (0x20b2f06b8). So this is the
	 * answer that decides something for the game, and it can be measured here without the
	 * game -- which is the only reason this block is in a capability sweep.
	 *
	 * -gpuCoreCount is answered by nothing in this port's chain (only Apple's own
	 * AGXA11FamilyDevice declares it; _MTLDevice has no entry). MetalFX guards it with
	 * respondsToSelector:, so it is a NO and not an abort -- and that guard is the only
	 * thing between this device and an unrecognized selector, which is worth printing
	 * separately from the verdict.
	 */
	printf("caps: -- MetalFX, which is the acceptance test's own capability gate --\n");
	printf("caps:   respondsToSelector(gpuCoreCount)           %s\n",
	       [dev respondsToSelector:sel_getUid("gpuCoreCount")] ? "yes" : "no");
	for (int spatial = 0; spatial < 2; spatial++) {
		const char *cls = spatial ? "MTLFXSpatialScalerDescriptor"
					  : "MTLFXTemporalScalerDescriptor";
		Class c = objc_lookUpClass(cls);

		if (c == Nil) {
			/* MetalFX is not linked by this program, so the class arrives only if
			 * something else in the process pulled it in; dlopen is deliberately not
			 * used here, because a framework loaded by hand is not the thing the game
			 * loads. A Nil here is a statement about this program, not the device. */
			printf("caps:   %s not in this process\n", cls);
			continue;
		}
		@try {
			BOOL ok = ((BOOL (*)(Class, SEL, id))objc_msgSend)(c,
			    sel_getUid("supportsDevice:"), dev);

			printf("caps:   +[%s supportsDevice:] %s\n", cls, ok ? "YES" : "no");
		} @catch (NSException *e) {
			printf("caps:   +[%s supportsDevice:] raised %s: %s\n", cls,
			       [[e name] UTF8String], [[e reason] UTF8String]);
		}
	}
	printf("caps: done\n");
	fflush(stdout);
	return 0;
}
