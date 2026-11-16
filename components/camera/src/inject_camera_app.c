/*
 * libhakios-camapp.dylib: Camera.app's preview given a size (iOS 26/27; etc/inject.conf names Camera).
 *
 * The preview is an AVSpatialOverCaptureVideoPreviewLayer whose one sublayer shows the session's image queue
 * (contents a CASlotProxy). AVFoundation sizes that sublayer from the over-capture geometry: the wide camera's
 * frame within the ultra wide's, from their base zoom factors -- and there is one camera here, whose camera
 * information has no base zoom factor (cameracaptured's preview stitcher reads it as 0 too). The size came out
 * 0x0 at the layer's centre (a layer dump on com.hakios.layers, logs/041316-run.log in hakios-sm7325): the frames
 * reached the image queue about 8 a second, cameracaptured and the window server did their work, and the preview
 * was black. A sublayer of the preview layer given an empty or NaN size takes the preview layer's bounds instead:
 * the picture fills the preview, as AVLayerVideoGravityResizeAspectFill would with one camera.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { double x, y, w, h; } rect;

static void
say(const char *format, ...)
{
	char line[300];
	va_list ap;
	int fd;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		write(fd, line, strlen(line));
		close(fd);
	}
}

static Class preview_class;

static bool
usable(rect r)
{
	return isfinite(r.x) && isfinite(r.y) && isfinite(r.w) && isfinite(r.h) && r.w > 0 && r.h > 0;
}

static void (*original_set_bounds)(id, SEL, rect);
static void (*original_set_frame)(id, SEL, rect);

static bool
is_preview(id layer)
{
	for (Class c = object_getClass(layer); c != Nil; c = class_getSuperclass(c)) {
		if (c == preview_class) {
			return true;
		}
	}
	return false;
}

static bool
fix(id self, rect *r, const char *what)
{
	static int told;
	id super;
	rect bounds;

	if (usable(*r) || preview_class == Nil) {
		return false;
	}
	super = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("superlayer"));
	if (super == nil || !is_preview(super)) {
		return false;
	}
	bounds = ((rect (*)(id, SEL))objc_msgSend)(super, sel_registerName("bounds"));
	if (!usable(bounds)) {
		return false;
	}
	if (!told) {
		told = 1;
		say("hakios: Camera: the preview's picture layer was given %s %g,%g %gx%g: the preview layer's %gx%g instead\n", what,
		    r->x, r->y, r->w, r->h, bounds.w, bounds.h);
	}
	*r = bounds;
	return true;
}

static void
set_bounds(id self, SEL sel, rect r)
{
	(void)fix(self, &r, "bounds");
	original_set_bounds(self, sel, r);
}

static void
set_frame(id self, SEL sel, rect r)
{
	(void)fix(self, &r, "frame");
	original_set_frame(self, sel, r);
}

/*
 * And where AVFoundation gives the sublayer no size at all (it stays at its first, 0x0), at the preview layer's
 * layout: each sublayer without a usable size gets the preview layer's bounds, centred.
 */
typedef void (*layout_fn)(id, SEL);
static layout_fn original_layouts[2];
static Class layout_classes[2];

static void
fix_sublayers(id self)
{
	static int told;
	rect bounds = ((rect (*)(id, SEL))objc_msgSend)(self, sel_registerName("bounds"));
	id sublayers = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("sublayers"));
	long count = sublayers != nil ? ((long (*)(id, SEL))objc_msgSend)(sublayers, sel_registerName("count")) : 0;

	if (!usable(bounds)) {
		return;
	}
	for (long i = 0; i < count; i++) {
		id sub = ((id (*)(id, SEL, long))objc_msgSend)(sublayers, sel_registerName("objectAtIndex:"), i);
		rect b = ((rect (*)(id, SEL))objc_msgSend)(sub, sel_registerName("bounds"));
		struct { double x, y; } centre = { bounds.x + bounds.w / 2, bounds.y + bounds.h / 2 };

		if (usable(b)) {
			continue;
		}
		if (told < 3) {
			told++;
			say("hakios: Camera: the preview's sublayer %s had bounds %g,%g %gx%g at layout: the preview's %gx%g instead\n",
			    object_getClassName(sub), b.x, b.y, b.w, b.h, bounds.w, bounds.h);
		}
		original_set_bounds(sub, sel_registerName("setBounds:"), (rect){ 0, 0, bounds.w, bounds.h });
		((void (*)(id, SEL, typeof(centre)))objc_msgSend)(sub, sel_registerName("setPosition:"), centre);
	}
}

static void
layout_0(id self, SEL sel)
{
	original_layouts[0](self, sel);
	fix_sublayers(self);
}

static void
layout_1(id self, SEL sel)
{
	original_layouts[1](self, sel);
	fix_sublayers(self);
}

typedef void (*add_fn)(id, SEL, id);
typedef void (*insert_fn)(id, SEL, id, unsigned);
static add_fn original_add;
static insert_fn original_insert;

static void
add_sublayer(id self, SEL sel, id layer)
{
	original_add(self, sel, layer);
	fix_sublayers(self);
}

static void
insert_sublayer(id self, SEL sel, id layer, unsigned index)
{
	original_insert(self, sel, layer, index);
	fix_sublayers(self);
}

static void
hook_layout(void)
{
	static const char *const names[2] = { "AVCaptureVideoPreviewLayer", "AVSpatialOverCaptureVideoPreviewLayer" };
	static const IMP with[2] = { (IMP)layout_0, (IMP)layout_1 };
	SEL layout = sel_registerName("layoutSublayers");

	for (int i = 0; i < 2; i++) {
		Class c = objc_getClass(names[i]);
		Method m = c != Nil ? class_getInstanceMethod(c, layout) : NULL;

		if (m == NULL) {
			continue;
		}
		/* (a method of its own, or one it inherits: the latter is added to the class, not replaced in CALayer) */
		if (class_addMethod(c, layout, with[i], method_getTypeEncoding(m))) {
			original_layouts[i] = (layout_fn)method_getImplementation(m);
		} else {
			original_layouts[i] = (layout_fn)method_setImplementation(m, with[i]);
		}
		layout_classes[i] = c;
	}
	{
		Class c = objc_getClass("AVCaptureVideoPreviewLayer");
		SEL add = sel_registerName("addSublayer:"), insert = sel_registerName("insertSublayer:atIndex:");
		Method a = c != Nil ? class_getInstanceMethod(c, add) : NULL, n = c != Nil ? class_getInstanceMethod(c, insert) : NULL;

		if (a != NULL) {
			original_add = (add_fn)method_getImplementation(a);
			if (!class_addMethod(c, add, (IMP)add_sublayer, method_getTypeEncoding(a))) {
				original_add = (add_fn)method_setImplementation(a, (IMP)add_sublayer);
			}
		}
		if (n != NULL) {
			original_insert = (insert_fn)method_getImplementation(n);
			if (!class_addMethod(c, insert, (IMP)insert_sublayer, method_getTypeEncoding(n))) {
				original_insert = (insert_fn)method_setImplementation(n, (IMP)insert_sublayer);
			}
		}
	}
}

__attribute__((constructor))
static void
camera_app_init(void)
{
	const char *name = getprogname();
	Class layer = objc_getClass("CALayer");
	Method bounds, frame;

	if (name == NULL || strcmp(name, "Camera") != 0 || layer == Nil) {
		return;
	}
	preview_class = objc_getClass("AVCaptureVideoPreviewLayer");
	bounds = class_getInstanceMethod(layer, sel_registerName("setBounds:"));
	frame = class_getInstanceMethod(layer, sel_registerName("setFrame:"));
	if (preview_class == Nil || bounds == NULL || frame == NULL) {
		say("hakios: Camera: no AVCaptureVideoPreviewLayer or CALayer setters here: the preview is left alone\n");
		return;
	}
	original_set_bounds = (void (*)(id, SEL, rect))method_setImplementation(bounds, (IMP)set_bounds);
	original_set_frame = (void (*)(id, SEL, rect))method_setImplementation(frame, (IMP)set_frame);
	hook_layout();
	say("hakios: Camera: a sublayer of the preview layer without a size takes the preview's\n");
}
