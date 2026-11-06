/*
 * render-ca-wallpaper: an iOS wallpaper of the layered kind (a .wallpaper directory of .ca packages,
 * as the iOS 16 collections are) drawn into one picture on this Mac, with the Mac's own QuartzCore:
 * the packages' Background, Foreground and Floating layers, in that order, at the wallpaper's own
 * size. For a phone whose PosterBoard cannot draw them (the Infinix, where they want Metal): the
 * picture goes into the image and SpringBoard is handed it (userland/src/inject_sb.c).
 *
 * usage: render-ca-wallpaper WALLPAPER_DIR OUT.jpg [WIDTH HEIGHT [QUALITY]]
 * QUALITY is JPEG's, 0 to 1 (0.82 when not given): the Infinix's boot partition has room for some
 * 15 KB of wallpaper, which 375x812 at 0.4 is -- and the iOS 16 ones, all soft gradients, lose
 * nothing to the eye at half the size, SpringBoard scaling them to the screen.
 * build: clang -fobjc-arc -framework Foundation -framework QuartzCore -framework ImageIO
 *        -framework CoreGraphics -o render-ca-wallpaper tools/render-ca-wallpaper.m
 */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>

@interface CAPackage : NSObject
+ (instancetype)packageWithContentsOfURL:(NSURL *)url type:(NSString *)type options:(NSDictionary *)options
                                   error:(NSError **)error;
@property (readonly) CALayer *rootLayer;
@end
extern NSString *const kCAPackageTypeCAMLBundle;

/* A layer and its subtree drawn into a bitmap of its own bounds (scaled), top-left origin. */
static CGContextRef
draw_layer(CALayer *layer, CGFloat scale, bool alpha_only)
{
	size_t w = (size_t)ceil(layer.bounds.size.width * scale), h = (size_t)ceil(layer.bounds.size.height * scale);
	CGContextRef context;

	if (w == 0 || h == 0) {
		return NULL;
	}
	if (alpha_only) {
		context = CGBitmapContextCreate(NULL, w, h, 8, 0, NULL, kCGImageAlphaOnly);
	} else {
		CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		context = CGBitmapContextCreate(NULL, w, h, 8, 0, space, kCGImageAlphaPremultipliedLast);
		CGColorSpaceRelease(space);
	}
	CGContextTranslateCTM(context, 0, (CGFloat)h);
	CGContextScaleCTM(context, scale, -scale);
	CGContextTranslateCTM(context, -layer.bounds.origin.x, -layer.bounds.origin.y);
	[layer renderInContext:context];
	return context;
}

/* A filter's name, as the archive gives it ("sourceAtop"), or nil. */
static NSString *
filter_name(id filter)
{
	if (filter == nil) {
		return nil;
	}
	@try {
		return [filter valueForKey:@"name"];
	} @catch (NSException *e) {
		return nil;
	}
}

/*
 * A sublayer whose compositingFilter is sourceAtop draws only where its parent already has alpha:
 * the iOS 16 wallpapers cut the head out of its HEIC that way ("Head Mask.HEIC" holding a
 * "Subtransform" with sourceAtop over "Head.HEIC"). -renderInContext: ignores compositing filters
 * and drew the whole rectangle, so the parent is drawn without them, each of them drawn on its own
 * and laid over with the same blend mode, and the result made the parent's contents.
 */
static void
flatten_source_atop(CALayer *layer, CGFloat scale)
{
	NSMutableArray *atop = [NSMutableArray array];

	for (CALayer *sub in layer.sublayers) {
		if ([filter_name(sub.compositingFilter) isEqualToString:@"sourceAtop"]) {
			[atop addObject:sub];
		}
	}
	if (atop.count == 0) {
		return;
	}
	for (CALayer *sub in atop) {
		sub.hidden = YES;
	}
	CGContextRef body = draw_layer(layer, scale, false);
	if (body == NULL) {
		return;
	}
	for (CALayer *sub in atop) {
		sub.hidden = NO;
		sub.compositingFilter = nil;
		CGContextRef over = draw_layer(sub, scale, false);
		if (over == NULL) {
			continue;
		}
		CGImageRef image = CGBitmapContextCreateImage(over);
		CGRect frame = sub.frame;
		CGContextSaveGState(body);
		CGContextSetBlendMode(body, kCGBlendModeSourceAtop);
		/* body's space is the layer's, y down; an image draws y up, so it is flipped back in place */
		CGContextTranslateCTM(body, frame.origin.x, frame.origin.y + frame.size.height);
		CGContextScaleCTM(body, 1, -1);
		CGContextDrawImage(body, CGRectMake(0, 0, frame.size.width, frame.size.height), image);
		CGContextRestoreGState(body);
		CGImageRelease(image);
		CGContextRelease(over);
	}
	CGImageRef flat = CGBitmapContextCreateImage(body);
	layer.sublayers = nil;
	layer.contents = (__bridge id)flat;
	layer.contentsGravity = kCAGravityResize;
	CGImageRelease(flat);
	CGContextRelease(body);
	printf("render-ca-wallpaper: %lu sourceAtop layer%s of %s laid over by hand\n", (unsigned long)atop.count,
	    atop.count == 1 ? "" : "s", layer.name ? layer.name.UTF8String : "a layer");
}

/*
 * -renderInContext: draws no masks (CALayer's documentation says so), and the iOS 16 wallpapers
 * mask their shapes: drawn as they are, a masked shape shows as the rectangle it is cut from. So
 * every masked layer is flattened first, deepest first: the layer and the mask each drawn into a
 * bitmap, the one cut by the other's alpha, and the result put back as the layer's contents with
 * its sublayers and mask taken away -- after which the tree draws as Core Animation would show it.
 */
static void
flatten_masks(CALayer *layer, CGFloat scale)
{
	for (CALayer *sub in [layer.sublayers copy]) {
		flatten_masks(sub, scale);
	}
	flatten_source_atop(layer, scale);
	CALayer *mask = layer.mask;
	if (mask == nil) {
		return;
	}
	flatten_masks(mask, scale);
	layer.mask = nil;
	CGContextRef body = draw_layer(layer, scale, false);
	if (body == NULL) {
		return;
	}
	size_t w = CGBitmapContextGetWidth(body), h = CGBitmapContextGetHeight(body);
	CGContextRef cut = CGBitmapContextCreate(NULL, w, h, 8, 0, NULL, kCGImageAlphaOnly);
	/* the mask in its parent's (this layer's) coordinates: its frame */
	CGContextTranslateCTM(cut, 0, (CGFloat)h);
	CGContextScaleCTM(cut, scale, -scale);
	CGContextTranslateCTM(cut, -layer.bounds.origin.x + mask.frame.origin.x,
	    -layer.bounds.origin.y + mask.frame.origin.y);
	CGContextTranslateCTM(cut, -mask.bounds.origin.x, -mask.bounds.origin.y);
	[mask renderInContext:cut];
	CGImageRef alpha = CGBitmapContextCreateImage(cut);
	CGImageRef image = CGBitmapContextCreateImage(body);
	CGDataProviderRef provider = CGImageGetDataProvider(alpha);
	CGImageRef stencil = CGImageMaskCreate(w, h, 8, 8, CGImageGetBytesPerRow(alpha), provider, NULL, false);
	/* an image mask paints where its samples are 0: the alpha, inverted, is the stencil */
	uint8_t *bytes = CGBitmapContextGetData(cut);
	for (size_t i = 0; i < CGBitmapContextGetBytesPerRow(cut) * h; i++) {
		bytes[i] = 255 - bytes[i];
	}
	CGImageRelease(stencil);
	CGImageRelease(alpha);
	alpha = CGBitmapContextCreateImage(cut);
	stencil = CGImageMaskCreate(w, h, 8, 8, CGImageGetBytesPerRow(alpha), CGImageGetDataProvider(alpha), NULL, false);
	CGImageRef masked = CGImageCreateWithMask(image, stencil);
	layer.sublayers = nil;
	layer.contents = (__bridge id)masked;
	layer.contentsGravity = kCAGravityResize;
	layer.backgroundColor = NULL;
	CGImageRelease(masked);
	CGImageRelease(stencil);
	CGImageRelease(alpha);
	CGImageRelease(image);
	CGContextRelease(cut);
	CGContextRelease(body);
	printf("render-ca-wallpaper: the mask of %s applied\n", layer.name ? layer.name.UTF8String : "a layer");
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 3) {
			fprintf(stderr, "usage: render-ca-wallpaper WALLPAPER_DIR OUT.jpg [WIDTH HEIGHT [QUALITY]]\n");
			return 1;
		}
		NSString *dir = [NSString stringWithUTF8String:argv[1]];
		size_t width = argc > 4 ? (size_t)atoi(argv[3]) : 0, height = argc > 4 ? (size_t)atoi(argv[4]) : 0;
		NSArray *names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil];
		NSMutableArray *layers = [NSMutableArray array];

		for (NSString *part in @[ @"_Background", @"_Foreground", @"_Floating" ]) {
			for (NSString *name in names) {
				if (![name hasSuffix:@".ca"] || [name rangeOfString:part].location == NSNotFound) {
					continue;
				}
				NSError *error = nil;
				CAPackage *package = [CAPackage packageWithContentsOfURL:[NSURL fileURLWithPath:
				    [dir stringByAppendingPathComponent:name]] type:kCAPackageTypeCAMLBundle options:nil
				    error:&error];
				if (package == nil || package.rootLayer == nil) {
					fprintf(stderr, "render-ca-wallpaper: %s: %s\n", name.UTF8String,
					    error.description.UTF8String);
					return 1;
				}
				[package.rootLayer layoutIfNeeded];
				flatten_masks(package.rootLayer, 3);
				[layers addObject:package.rootLayer];
				printf("render-ca-wallpaper: %s, %.0fx%.0f\n", name.UTF8String,
				    package.rootLayer.bounds.size.width, package.rootLayer.bounds.size.height);
			}
		}
		if (layers.count == 0) {
			fprintf(stderr, "render-ca-wallpaper: no .ca packages in %s\n", argv[1]);
			return 1;
		}
		CGSize size = ((CALayer *)layers[0]).bounds.size;
		if (width == 0) {
			width = (size_t)size.width;
			height = (size_t)size.height;
		}
		CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		CGContextRef context = CGBitmapContextCreate(NULL, width, height, 8, 0, space,
		    kCGImageAlphaPremultipliedLast);
		/* CALayer draws with its origin at the top left */
		CGContextTranslateCTM(context, 0, (CGFloat)height);
		CGContextScaleCTM(context, (CGFloat)width / size.width, -(CGFloat)height / size.height);
		for (CALayer *layer in layers) {
			[layer layoutIfNeeded];
			[layer renderInContext:context];
		}
		CGImageRef image = CGBitmapContextCreateImage(context);
		CGImageDestinationRef out = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL
		    fileURLWithPath:[NSString stringWithUTF8String:argv[2]]], CFSTR("public.jpeg"), 1, NULL);
		CGImageDestinationAddImage(out, image, (__bridge CFDictionaryRef)@{
			(__bridge NSString *)kCGImageDestinationLossyCompressionQuality: @(argc > 5 ? atof(argv[5]) : 0.82) });
		bool ok = CGImageDestinationFinalize(out);
		printf("render-ca-wallpaper: %zux%zu into %s: %s\n", width, height, argv[2], ok ? "written" : "FAILED");
		return ok ? 0 : 1;
	}
}
