/*
 * Metal over Vulkan on the phone, the smallest whole round: UniXNU's AdrenoMetal frontend and its metal2vulkan runtime
 * (tools/mk-m2v.sh) over Turnip (tools/mk-turnip.sh), linked into this program and opened as the plug-in would open it
 * (native_bridge_metal_open), with no window server in between. A buffer filled by a blit, a texture cleared by a render
 * pass and read back, each checked; then the clear a hundred times, timed. RC_JOBS=rc.jobs.m2vtest runs it (an image
 * built with TURNIP=1 M2V=1).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <signal.h>
#include <stdio.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

id native_bridge_metal_open(const char *loader, const char *cache, uint64_t registryID, void (*missing)(const char *, const char *));
size_t native_bridge_metal_status(char *out, size_t size);
uint64_t native_bridge_metal_first_problem(char *out, size_t size);

#define TURNIP "/redwood/lib/libvulkan_freedreno.dylib"
#define CACHE "/private/var/tmp/m2v"

static double
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void
missing(const char *class, const char *selector)
{
	printf("m2v_test: missing -[%s %s]\n", class, selector);
}

static void
where(const char *what, uintptr_t a)
{
	Dl_info info;

	if (dladdr((void *)a, &info) && info.dli_fname != NULL)
		printf("m2v_test:   %s %#lx = %s+%#lx (%s)\n", what, (unsigned long)a, info.dli_fname,
		    (unsigned long)(a - (uintptr_t)info.dli_fbase), info.dli_sname ? info.dli_sname : "?");
	else
		printf("m2v_test:   %s %#lx\n", what, (unsigned long)a);
}

static void
crashed(int sig, siginfo_t *si, void *context)
{
	ucontext_t *uc = context;

	printf("m2v_test: signal %d at address %p\n", sig, si->si_addr);
	where("pc", (uintptr_t)uc->uc_mcontext->__ss.__pc);
	where("lr", (uintptr_t)uc->uc_mcontext->__ss.__lr);
	uintptr_t fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
	for (int i = 0; i < 16 && fp != 0 && (fp & 7) == 0; i++) {
		where("frame", ((uintptr_t *)fp)[1]);
		fp = ((uintptr_t *)fp)[0];
	}
	_exit(128 + sig);
}

static int
status(int failed)
{
	char line[512] = "", problem[256] = "";

	native_bridge_metal_status(line, sizeof line);
	printf("m2v_test: status %s\n", line);
	if (native_bridge_metal_first_problem(problem, sizeof problem))
		printf("m2v_test: first problem: %s\n", problem);
	printf("m2v_test: %s\n", failed ? "FAIL" : "PASS");
	return failed;
}

int
main(void)
{
	struct sigaction sa = { .sa_sigaction = crashed, .sa_flags = SA_SIGINFO };
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGTRAP, &sa, NULL);
	setvbuf(stdout, NULL, _IONBF, 0);
	dup2(1, 2);                     /* the runtime's and Turnip's messages (stderr) beside ours */
	setenv("TU_DEBUG", "startup", 0);
	mkdir("/private/var/tmp", 0777);
	mkdir(CACHE, 0700);

	@autoreleasepool {
		double t0 = now_ms();
		id<MTLDevice> device = native_bridge_metal_open(TURNIP, CACHE, 0, missing);
		if (device == nil) {
			printf("m2v_test: no device\n");
			return status(1);
		}
		printf("m2v_test: device \"%s\" (class %s) in %.1f ms\n", device.name.UTF8String,
		    object_getClassName(device), now_ms() - t0);

		id<MTLCommandQueue> queue = [device newCommandQueue];
		const NSUInteger words = 1u << 20;
		id<MTLBuffer> buffer = [device newBufferWithLength:words * 4 options:MTLResourceStorageModeShared];
		memset(buffer.contents, 0, words * 4);
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit fillBuffer:buffer range:NSMakeRange(0, words * 4) value:0x5a];
		[blit endEncoding];
		t0 = now_ms();
		[cb commit];
		[cb waitUntilCompleted];
		const uint32_t *w = buffer.contents;
		NSUInteger right = 0;
		for (NSUInteger i = 0; i < words; i++)
			right += w[i] == 0x5a5a5a5a;
		printf("m2v_test: blit fill: %lu of %lu words right in %.2f ms (status %ld)%s\n", (unsigned long)right,
		    (unsigned long)words, now_ms() - t0, (long)cb.status, right == words ? "" : " -- WRONG");
		if (right != words)
			return status(1);

		const NSUInteger width = 1080, height = 2400;
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		    width:width height:height mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModeShared;
		id<MTLTexture> texture = [device newTextureWithDescriptor:td];
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = texture;
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		rp.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.5, 0.0, 1.0);
		cb = [queue commandBuffer];
		[[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
		t0 = now_ms();
		[cb commit];
		[cb waitUntilCompleted];
		double first = now_ms() - t0;
		uint32_t *pixels = malloc(width * height * 4);
		[texture getBytes:pixels bytesPerRow:width * 4 fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
		NSUInteger good = 0;
		for (NSUInteger i = 0; i < width * height; i++)
			good += (pixels[i] & 0xffffff) == 0xff8000 || (pixels[i] & 0xffffff) == 0xff7f00;
		printf("m2v_test: clear of %lux%lu: %lu of %lu pixels orange in %.2f ms (first pixel %#x)%s\n", (unsigned long)width,
		    (unsigned long)height, (unsigned long)good, (unsigned long)(width * height), first, pixels[0],
		    good == width * height ? "" : " -- WRONG");

		double best = 1e9, sum = 0;
		for (int i = 0; i < 100; i++) {
			cb = [queue commandBuffer];
			[[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
			double a = now_ms();
			[cb commit];
			[cb waitUntilCompleted];
			double d = now_ms() - a;
			sum += d;
			if (d < best)
				best = d;
		}
		printf("m2v_test: 100 full-screen clears, each committed and waited for: %.3f ms on average, %.3f at best\n",
		    sum / 100, best);
		return status(good != width * height);
	}
}
