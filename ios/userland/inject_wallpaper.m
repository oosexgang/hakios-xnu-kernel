/*
 * libhakios-wallpaper.dylib: inside CollectionsPoster, the WallpaperKit extension that draws the collections poster.
 *
 * The default wallpaper here (ios/tools/mkimage26.py default_wallpaper(): Stripe Blue) is SwiftUI's: a layer of six
 * CAShapeLayers (SwiftUI's ColorShapeLayer), a stripe each. The window server draws a shape layer by CoreAnimation's
 * path renderer, every frame, though the stripes never change: on the home screen's main pass 6.8 of its 12.3 ms on
 * the GPU (metal_gmem_off=2048, logs/090904-run.log), and while a page moved the pass went past a frame's time, so that
 * CoreAnimation kept to 30 frames a second.
 *
 * A layer whose sublayers are two shape layers or more is rasterized here (shouldRasterize, at its shapes' own scale):
 * the window server draws its stripes once into a texture of its own and composites that, until a shape changes. The
 * layers are looked at every second for the first minute and every five after it, as SwiftUI may make them again (the
 * lock screen's stripes and the home screen's differ in height). Boot-arg wallpaper_raster=0 leaves them alone.
 */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* (to the console itself: an extension's standard error goes nowhere the run's log sees) */
static void
say(NSString *text)
{
	static int fd = -2;
	if (fd == -2) fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	NSData *d = [[text stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding];
	write(fd >= 0 ? fd : 2, [d bytes], [d length]);
}

/* LAYER and the layers under it: those with two shape layers or more among their sublayers rasterized; how many now */
static unsigned
rasterize(CALayer *layer, unsigned depth)
{
	unsigned shapes = 0, made = 0;
	CGFloat scale = 0;

	if (layer == nil || depth > 24) return 0;
	for (CALayer *s in [layer sublayers]) {
		if ([s isKindOfClass:[CAShapeLayer class]]) shapes++, scale = MAX(scale, [s contentsScale]);
	}
	if (shapes >= 2 && ![layer shouldRasterize]) {
		[layer setRasterizationScale:scale > 0 ? scale : 2];
		[layer setShouldRasterize:YES];
		made++;
		CGRect b = [layer bounds];
		say([NSString stringWithFormat:@"hakios: wallpaper: %u shape layers of a %.0fx%.0f layer rasterized at scale %.1f", shapes, b.size.width,
			 b.size.height, [layer rasterizationScale]]);
	}
	for (CALayer *s in [layer sublayers]) made += rasterize(s, depth + 1);
	return made;
}

static void
look(void)
{
	Class window = NSClassFromString(@"UIWindow");
	SEL all = NSSelectorFromString(@"allWindowsIncludingInternalWindows:onlyVisibleWindows:");

	if (window == Nil || ![window respondsToSelector:all]) return;
	for (id w in ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)(window, all, YES, NO)) {
		rasterize([w layer], 0);
	}
}

static BOOL
off(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;

	return sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "wallpaper_raster=0") != NULL;
}

__attribute__((constructor)) static void
start(void)
{
	if (off()) {
		say(@"hakios: wallpaper: left as it is (wallpaper_raster=0)");
		return;
	}
	static dispatch_source_t timer;
	static unsigned ticks;
	timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
	dispatch_source_set_event_handler(timer, ^{
		/* (every second for a minute, then every fifth) */
		if (++ticks <= 60 || ticks % 5 == 0) look();
	});
	dispatch_resume(timer);
}
