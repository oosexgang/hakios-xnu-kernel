/*
 * libhakios-wallpaper.dylib: inside CollectionsPoster, the WallpaperKit extension that draws the collections poster.
 *
 * The default wallpaper here (darwin/tools/mkimage26.py default_wallpaper(): Stripe Yellow, or the colour WALLPAPER= names) is SwiftUI's: a layer of six
 * CAShapeLayers (SwiftUI's ColorShapeLayer), a stripe each. The window server draws a shape layer by CoreAnimation's
 * path renderer, every frame, though the stripes never change: on the home screen's main pass 6.8 of its 12.3 ms on
 * the GPU (metal_gmem_off=2048, logs/090904-run.log), and while a page moved the pass went past a frame's time, so that
 * CoreAnimation kept to 30 frames a second.
 *
 * A layer whose sublayers are two shape layers or more has its stripes drawn here once, by the processor, into a picture
 * the layer shows, the shape layers hidden (picture() below): the window server composites a texture. The layers are
 * looked at every second for the first minute and every five after it, as SwiftUI may make them again (the lock
 * screen's stripes and the home screen's differ in height), and a picture is drawn again when a shape changes.
 * Boot-arg wallpaper_raster=1 rasterizes them instead (shouldRasterize, as before), wallpaper_raster=0 leaves them alone.
 */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
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

/*
 * The default (wallpaper_raster=2): instead of shouldRasterize, the stripes drawn once by the processor into a picture that
 * the layer itself shows, its shape layers hidden. A rasterized layer is drawn again by the window server, through the path
 * renderer, whenever it thinks the rasterization stale -- every frame while an application opens or closes and the
 * wallpaper moves under it: a pass of 8 draws over the whole screen, 22 ms of the GPU's in GMEM, 37 in sysmem
 * (logs/085726-run.log), which held those frames to 22-48 a second. A picture is a texture the window server scales like
 * any other. Drawn again when a shape changes (what the shapes are is looked at each time the layers are).
 */
static int
mode(void)
{
	static int m = -1;
	if (m < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		m = 2;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "wallpaper_raster=")) != NULL) {
			m = atoi(at + strlen("wallpaper_raster="));
		}
	}
	return m;
}

/* what LAYER's shapes are: its bounds, then their paths' extents, frames, colours and opacities, in their order */
static NSString *
shapesOf(CALayer *layer)
{
	const CGRect b = [layer bounds];
	NSMutableString *sig = [NSMutableString stringWithFormat:@"%.2f %.2f %.2f %.2f:", b.origin.x, b.origin.y, b.size.width, b.size.height];

	for (CALayer *l in [layer sublayers]) {
		if (![l isKindOfClass:[CAShapeLayer class]]) continue;
		CAShapeLayer *s = (CAShapeLayer *)l;
		CGRect box = [s path] != NULL ? CGPathGetBoundingBox([s path]) : CGRectNull;
		CGColorRef c = [s fillColor];
		const CGRect f = [s frame];
		[sig appendFormat:@"%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.3f", box.origin.x, box.origin.y, box.size.width, box.size.height,
		     f.origin.x, f.origin.y, f.size.width, f.size.height, [s opacity]];
		for (size_t i = 0; c != NULL && i < CGColorGetNumberOfComponents(c); i++) [sig appendFormat:@" %.4f", CGColorGetComponents(c)[i]];
		[sig appendString:@";"];
	}
	return sig;
}

/* (the most pixels a picture is drawn with, 16 M: 64 MB of it; a layer larger than that is left as it is) */
#define PICTURE_MAX (16.0 * 1024 * 1024)

static unsigned
picture(CALayer *layer, unsigned depth)
{
	static NSMapTable *drawn;
	unsigned shapes = 0, made = 0;
	CGFloat scale = 0;

	if (layer == nil || depth > 24) return 0;
	/* (kept: this file is built without ARC, and the table as made is autoreleased -- stored as it was, it was freed with
	 * the pool, and the next look at it took CollectionsPoster down in objc_msgSend, again and again) */
	if (drawn == nil) drawn = [[NSMapTable weakToStrongObjectsMapTable] retain];
	for (CALayer *s in [layer sublayers]) {
		if ([s isKindOfClass:[CAShapeLayer class]]) shapes++, scale = MAX(scale, [s contentsScale]);
	}
	NSString *sig = shapes >= 2 ? shapesOf(layer) : nil;
	NSString *had = [drawn objectForKey:layer];
	if (sig == nil) {
		/* (a layer drawn before that has not got its shapes any more: its picture taken away, the shape layers it has shown) */
		if (had != nil) {
			[CATransaction begin];
			[CATransaction setDisableActions:YES];
			[layer setContents:nil];
			for (CALayer *s in [layer sublayers]) {
				if ([s isKindOfClass:[CAShapeLayer class]]) [s setHidden:NO];
			}
			[CATransaction commit];
			[drawn removeObjectForKey:layer];
		}
	} else if ([sig isEqualToString:had]) {
		/* (a layer that keeps its picture: its shape layers hidden again, should SwiftUI have shown them since) */
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		for (CALayer *s in [layer sublayers]) {
			if ([s isKindOfClass:[CAShapeLayer class]] && ![s isHidden]) [s setHidden:YES];
		}
		[CATransaction commit];
	} else {
		const CGRect b = [layer bounds];
		const CGFloat k = scale > 0 ? scale : 2;
		/* (a size that is not a number, is nothing or is past PICTURE_MAX is not drawn) */
		const double fw = ceil(b.size.width * k), fh = ceil(b.size.height * k);
		const BOOL fits = isfinite(fw) && isfinite(fh) && fw > 0 && fh > 0 && fw * fh <= PICTURE_MAX;
		const size_t w = fits ? (size_t)fw : 0, h = fits ? (size_t)fh : 0;
		CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
		CGContextRef cx = w && h ? CGBitmapContextCreate(NULL, w, h, 8, 0, rgb, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little) : NULL;

		CGColorSpaceRelease(rgb);
		if (cx != NULL) {
			[CATransaction begin];
			[CATransaction setDisableActions:YES];
			/* (the shapes shown and the picture taken away for the drawing: renderInContext draws the layers as set) */
			[layer setContents:nil];
			[layer setShouldRasterize:NO];
			for (CALayer *s in [layer sublayers]) {
				if ([s isKindOfClass:[CAShapeLayer class]]) [s setHidden:NO];
			}
			CGContextTranslateCTM(cx, 0, (CGFloat)h);
			CGContextScaleCTM(cx, k, -k);
			CGContextTranslateCTM(cx, -b.origin.x, -b.origin.y);
			[layer renderInContext:cx];
			CGImageRef image = CGBitmapContextCreateImage(cx);
			[layer setContents:(__bridge id)image];
			[layer setContentsScale:k];
			for (CALayer *s in [layer sublayers]) {
				if ([s isKindOfClass:[CAShapeLayer class]]) [s setHidden:YES];
			}
			[CATransaction commit];
			CGImageRelease(image);
			CGContextRelease(cx);
			[drawn setObject:shapesOf(layer) forKey:layer];
			made++;
			say([NSString stringWithFormat:@"hakios: wallpaper: %u shape layers of a %.0fx%.0f layer drawn into a %zux%zu picture", shapes,
				 b.size.width, b.size.height, w, h]);
		}
	}
	for (CALayer *s in [layer sublayers]) made += picture(s, depth + 1);
	return made;
}

static void
look(void)
{
	Class window = NSClassFromString(@"UIWindow");
	SEL all = NSSelectorFromString(@"allWindowsIncludingInternalWindows:onlyVisibleWindows:");

	NSMutableArray *roots = [NSMutableArray array];

	if (window != Nil && [window respondsToSelector:all]) {
		for (id w in ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)(window, all, YES, NO)) {
			if ([w layer] != nil) [roots addObject:[w layer]];
		}
	}
	/* ... and every layer tree this process hands the window server outside a window: the poster's second instance draws
	 * the stripes it zooms while an application opens (896x1856) in a context of its own (+[CAContext allContexts]) */
	Class context = NSClassFromString(@"CAContext");
	SEL contexts = NSSelectorFromString(@"allContexts");
	if (context != Nil && [context respondsToSelector:contexts]) {
		for (id c in ((id (*)(id, SEL))objc_msgSend)(context, contexts)) {
			CALayer *l = [c respondsToSelector:@selector(layer)] ? ((CALayer * (*)(id, SEL))objc_msgSend)(c, @selector(layer)) : nil;
			if (l != nil && ![roots containsObject:l]) [roots addObject:l];
		}
	}
	/* (what was found, said when it changes: how many trees, and each layer with shape layers under it) */
	{
		static NSString *said;
		NSMutableString *now = [NSMutableString stringWithFormat:@"%lu trees", (unsigned long)[roots count]];
		NSMutableArray *stack = [NSMutableArray arrayWithArray:roots];
		while ([stack count] != 0 && [now length] < 900) {
			CALayer *l = [stack lastObject];
			unsigned shapes = 0;
			[stack removeLastObject];
			for (CALayer *s in [l sublayers]) {
				if ([s isKindOfClass:[CAShapeLayer class]]) shapes++;
				[stack addObject:s];
			}
			if (shapes != 0) [now appendFormat:@"; %u shapes under a %.0fx%.0f %s", shapes, [l bounds].size.width, [l bounds].size.height, class_getName([l class])];
		}
		if (![now isEqualToString:said]) {
			[said release];
			said = [now copy];
			say([NSString stringWithFormat:@"hakios: wallpaper: %s[%d]: %@", getprogname(), getpid(), now]);
		}
	}
	for (CALayer *l in roots) {
		if (mode() == 2) {
			picture(l, 0);
		} else {
			rasterize(l, 0);
		}
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
