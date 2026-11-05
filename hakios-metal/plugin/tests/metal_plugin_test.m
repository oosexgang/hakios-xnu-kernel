/*
 * The Metal plug-in's objects (userland/src/HakiosMetal.m) on this Mac, against the Mac's own Metal: the same source built for
 * macOS, its device made by hand the way Metal makes it (-initWithAcceleratorPort:), and what a program does with buffers and
 * libraries checked. tools/metal-plugin-test.sh builds and runs it, under the address sanitizer.
 *
 * What this shows and what it does not. Metal's private classes on macOS are not Darwin 16.5's (the plug-in takes _MTLResource,
 * _MTLLibrary and _MTLFunction where they exist, and both systems have them), so this checks the plug-in's own logic -- the
 * classes it makes at run time, their state, the buffers' memory, the reading of a .metallib, every object freed -- not that
 * Darwin's Metal is content with them. That is metal_test's to say, in QEMU (userland/rc.jobs.metal).
 *
 * The binary must be called metal_test: the plug-in shows its device to that program only (metal=1, the level a Mac's
 * kern.bootargs gives, having no metal= in it).
 *
 *   usage: metal_test [FILE.metallib ...]      libraries to read besides the one made here
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void
check(const char *what, BOOL ok)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	failures += !ok;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		Class cls = objc_getClass("HakiosMetalDevice");
		id<MTLDevice> device;

		check("the plug-in registered HakiosMetalDevice, a subclass of _MTLDevice",
		    cls != Nil && class_getSuperclass(cls) == objc_getClass("_MTLDevice"));
		if (cls == Nil) {
			return 1;
		}
		device = ((id (*)(id, SEL, unsigned int))objc_msgSend)([cls alloc], sel_registerName("initWithAcceleratorPort:"), 7);
		check("-initWithAcceleratorPort: gives a device that conforms to MTLDevice", device != nil && [(id)device conformsToProtocol:@protocol(MTLDevice)]);
		check("it shares the system's memory", [device hasUnifiedMemory]);
		check("what CoreImage asks of a device's limits is answered (no memory of its own, the system's shared)",
		    [(id)device respondsToSelector:sel_registerName("sharedMemorySize")] &&
		    ((uint64_t (*)(id, SEL))objc_msgSend)((id)device, sel_registerName("sharedMemorySize")) == [[NSProcessInfo processInfo] physicalMemory] &&
		    ((uint64_t (*)(id, SEL))objc_msgSend)((id)device, sel_registerName("dedicatedMemorySize")) == 0 && ![device isLowPower] &&
		    ![device isRemovable] && ![device isHeadless]);

		MTLSize most = [device maxThreadsPerThreadgroup];
		check("maxThreadsPerThreadgroup: the Adreno's 1024 (it was zero: a stub)", most.width == 1024 && most.height == 1024 && most.depth == 1024);

		/* buffers */
		id<MTLBuffer> b = [device newBufferWithLength:10000 options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
		check("newBufferWithLength: a buffer of that length, zeroed, conforming to MTLBuffer", b != nil && [b length] == 10000 &&
		    ((uint8_t *)[b contents])[9999] == 0 && [(id)b conformsToProtocol:@protocol(MTLBuffer)]);
		memset([b contents], 0x5a, [b length]);
		check("what is written through -contents reads back", ((uint8_t *)[b contents])[0] == 0x5a && ((uint8_t *)[b contents])[9999] == 0x5a);
		check("its options: shared storage, write-combined", [b storageMode] == MTLStorageModeShared &&
		    [b cpuCacheMode] == MTLCPUCacheModeWriteCombined && [b allocatedSize] >= 10000 && [b device] == device);
		[b setLabel:@"a buffer"];
		check("a label is kept", [[b label] isEqualToString:@"a buffer"]);
		[b release];
		const char text[] = "copied, not kept";
		id<MTLBuffer> c = [device newBufferWithBytes:text length:sizeof(text) options:0];
		check("newBufferWithBytes: a copy", c != nil && [c contents] != text && memcmp([c contents], text, sizeof(text)) == 0);
		[c release];
		check("a buffer longer than maxBufferLength is refused", [device newBufferWithLength:[device maxBufferLength] + 1 options:0] == nil);
		__block int freed = 0;
		void *page = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
		id<MTLBuffer> n = [device newBufferWithBytesNoCopy:page length:16384 options:0 deallocator:^(void *p, NSUInteger l) {
			freed = (p == page && l == 16384);
			munmap(p, l);
		}];
		check("newBufferWithBytesNoCopy: the caller's memory itself", n != nil && [n contents] == page);
		[n release];
		check("... and its deallocator is called when the buffer goes", freed == 1);

		/* libraries */
		NSError *error = nil;
		const char junk[] = "not a metallib at all, and long enough to have a header's worth of bytes in it......";
		dispatch_data_t data = dispatch_data_create(junk, sizeof(junk), NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
		id<MTLLibrary> none = [device newLibraryWithData:data error:&error];
		check("newLibraryWithData: on bytes that are not a metallib: nil, and an error that says why", none == nil &&
		    [[error domain] isEqualToString:MTLLibraryErrorDomain] && [[error localizedDescription] containsString:@"not a metallib"]);
		dispatch_release(data);
		error = nil;
		check("newLibraryWithURL: of no file: nil, and an error", [device newLibraryWithURL:[NSURL fileURLWithPath:@"/nonexistent.metallib"]
		    error:&error] == nil && error != nil);
		for (int i = 1; i < argc; i++) {
			NSString *path = [NSString stringWithUTF8String:argv[i]];
			id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
			NSArray *names = [library functionNames];
			char label[512];

			/* (a library may have no function at all: SafariSharedUI's default.metallib is 118 bytes of header) */
			snprintf(label, sizeof(label), "%s: a library with its functions' names (%lu)", argv[i], (unsigned long)[names count]);
			check(label, library != nil && names != nil && [(id)library conformsToProtocol:@protocol(MTLLibrary)]);
			if (library == nil || [names count] == 0) {
				continue;
			}
			unsigned typed = 0, bitcode = 0;
			for (NSString *name in names) {
				id<MTLFunction> f = [library newFunctionWithName:name];
				NSData *module = [(id)f performSelector:sel_registerName("hakiosBitcode")];

				typed += f != nil && [[f name] isEqualToString:name] && [f functionType] >= MTLFunctionTypeVertex && [f device] == device;
				/* LLVM bitcode starts with "BC" 0xC0DE */
				bitcode += [module length] > 4 && memcmp([module bytes], "BC\xc0\xde", 4) == 0;
				[f release];
			}
			snprintf(label, sizeof(label), "%s: every function by its name, with its type, and its module is LLVM bitcode", argv[i]);
			check(label, typed == [names count] && bitcode == [names count]);
			check("a name the library does not have: nil", [library newFunctionWithName:@"no such function"] == nil);
			[library release];
		}
		/* the first library again, as a universal file (an Apple-GPU slice, then the AIR64 one) given as data */
		if (argc > 1) {
			NSData *thin = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]];
			NSMutableData *fat = [NSMutableData dataWithLength:4096];
			uint8_t *h = [fat mutableBytes];
			uint32_t length = (uint32_t)[thin length];
			const uint8_t header[] = { 0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 2, 0x01, 0, 0, 0x13, 0, 0, 0, 0, 0, 0, 0x08, 0, 0, 0, 0, 0x10, 0, 0, 0, 0,
				0x01, 0, 0, 0x17, 0, 0, 0, 0, 0, 0, 0x10, 0, (uint8_t)(length >> 24), (uint8_t)(length >> 16), (uint8_t)(length >> 8),
				(uint8_t)length, 0, 0, 0, 0 };

			memcpy(h, header, sizeof(header));
			[fat appendData:thin];
			dispatch_data_t d = dispatch_data_create([fat bytes], [fat length], NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
			id<MTLLibrary> universal = [device newLibraryWithData:d error:&error];
			id<MTLLibrary> plain = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
			check("a universal metallib given as data: its AIR64 slice, the same functions as the thin file",
			    universal != nil && [[universal functionNames] isEqualToArray:[plain functionNames]]);
			dispatch_release(d);
			[universal release];
			[plain release];
		}
		/* the queue: without a GPU it makes no command buffers; with one (GPU=1: the core's fake one) command buffers and blits,
		 * as metal_test asks for them on the phone -- and every object let go of afterwards, which is where the phone's faulted */
		id<MTLCommandQueue> queue = [device newCommandQueue];
		check("newCommandQueue: a queue, conforming to MTLCommandQueue", queue != nil && [(id)queue conformsToProtocol:@protocol(MTLCommandQueue)]);
		if (getenv("HAKIOS_METAL_FAKE") == NULL) {
			@autoreleasepool {
				check("without a GPU the queue makes no command buffers: nil", [queue commandBuffer] == nil);
			}
		} else if (queue != nil) {
			const NSUInteger n = 1 << 20;
			id<MTLBuffer> a = [device newBufferWithLength:n options:MTLResourceStorageModeShared];
			id<MTLBuffer> c = [device newBufferWithLength:n options:MTLResourceStorageModeShared];
			__block int handled = 0;

			@autoreleasepool {
				id<MTLCommandBuffer> cb = [queue commandBuffer];
				check("a command buffer from the GPU's queue", cb != nil && [cb commandQueue] == queue);
				uint32_t *pa = [a contents];
				for (NSUInteger i = 0; i < n / 4; i++) {
					pa[i] = (uint32_t)(i * 2654435761u);
				}
				id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
				[blit copyFromBuffer:a sourceOffset:4096 toBuffer:c destinationOffset:8192 size:500000];
				[blit fillBuffer:c range:NSMakeRange(600000, 4096) value:0xa5];
				[blit endEncoding];
				[cb addCompletedHandler:^(id<MTLCommandBuffer> done) { handled = [done status] == MTLCommandBufferStatusCompleted ? 1 : -1; }];
				[cb commit];
				[cb waitUntilCompleted];
				uint8_t *pc = [c contents];
				BOOL filled = YES;
				for (NSUInteger i = 600000; i < 604096; i++) {
					filled = filled && pc[i] == 0xa5;
				}
				check("blits: completed, the handler ran, the copy and the fill are right and nothing else changed",
				    [cb status] == MTLCommandBufferStatusCompleted && handled == 1 &&
				    memcmp(pc + 8192, (uint8_t *)pa + 4096, 500000) == 0 && filled && pc[8191] == 0 && pc[8192 + 500000] == 0);
			}
			[a release];
			[c release];
		}
		[queue release];
		check("the queue (and its command buffer, encoder and buffers) let go of: no fault", YES);
		[device release];
	}
	printf("metal_plugin_test: %s\n", failures ? "FAILED" : "all passed");
	return failures != 0;
}
