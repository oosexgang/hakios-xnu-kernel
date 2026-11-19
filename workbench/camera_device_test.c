/*
 * Host tests of Darwin's capture device (components/camera/src/camera_device.c), the file itself included here with the
 * picture processor it is built with (camera_isp.c): what it is given of CoreFoundation, CoreMedia, CoreVideo, libdispatch
 * and the vendor's dlsym() is this file's (just enough of each, every CF object counted), and /dev/hakios-camera is a
 * kernel of the test's, read and written through the file's own open(), read(), write() and close(). The device is made
 * with its create function, as the capture stack's vendor makes it; the scenarios set its streams' properties, start and
 * stop them as the capture graph does, and look at what the kernel saw and what was left allocated. Darwin's headers are
 * not on a Linux host: workbench/run.sh builds this against empty ones of the same names, with clang for the file's one
 * block type (-fblocks; the handler given here is a block laid out by hand, so no blocks runtime is needed).
 *
 *   camera_device_test               the scenarios' names
 *   camera_device_test SCENARIO      one scenario a process
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* ---- the allocations of the two files, counted ---------------------------------------------------------------- */

static long outstanding;                /* their malloc()s and calloc()s less their free()s */

static void *
counted(void *p)
{
	if (p != NULL) {
		__atomic_add_fetch(&outstanding, 1, __ATOMIC_RELAXED);
	}
	return p;
}

static void *counted_malloc(size_t n) { return counted(malloc(n)); }
static void *counted_calloc(size_t n, size_t size) { return counted(calloc(n, size)); }

static void
counted_free(void *p)
{
	if (p != NULL) {
		__atomic_sub_fetch(&outstanding, 1, __ATOMIC_RELAXED);
	}
	free(p);
}

/* ---- CoreFoundation, CoreMedia and CoreVideo: one kind of object, counted ------------------------------------- */

typedef unsigned char Boolean;
typedef long CFIndex;
typedef unsigned long CFTypeID;
typedef int32_t OSStatus;
typedef int32_t CVReturn;
typedef uint32_t CFStringEncoding;
typedef uint32_t CVAttachmentMode, CMAttachmentMode;
typedef uint64_t CVPixelBufferLockFlags;
typedef const void *CFTypeRef;
typedef const struct cf *CFStringRef, *CFNumberRef, *CFBooleanRef, *CFDictionaryRef, *CFArrayRef, *CFAllocatorRef, *CFSetRef;
typedef struct cf *CFMutableDictionaryRef, *CFMutableSetRef, *CVPixelBufferRef, *CVImageBufferRef, *CVPixelBufferPoolRef,
    *CMSampleBufferRef, *CMClockRef, *CMVideoFormatDescriptionRef;
typedef enum { kCFNumberSInt64Type = 4, kCFNumberFloatType = 5, kCFNumberDoubleType = 6 } CFNumberType;
typedef void (*CFDictionaryApplierFunction)(const void *key, const void *value, void *context);
typedef struct {
	int64_t value;
	int32_t timescale;
	uint32_t flags;
	int64_t epoch;
} CMTime;
typedef void *dispatch_queue_t;
typedef struct {
	uint32_t numer, denom;
} mach_timebase_info_data_t;

#define CF_MAGIC                0x43464f42      /* (a block, or anything else not made here, has none: never counted) */
enum { T_STRING = 1, T_NUMBER, T_BOOLEAN, T_DICTIONARY, T_ARRAY, T_SET, T_POOL, T_PIXEL_BUFFER, T_CLOCK, T_OTHER, T_DERIVED };

struct cf {
	uint32_t        magic;
	CFTypeID        type;
	int             refs;                   /* -1: a constant, never freed */
	double          number;
	int64_t         integer;
	const char *    string;
	CFIndex         count, capacity;
	const void **   keys, **values;         /* a dictionary's (a set's keys only), an array's values */
	struct cf *     attachments;
	uint32_t        width, height, format;  /* a pool's and its pixel buffers' */
	uint8_t *       plane[2];
	size_t          row_bytes[2], plane_height[2];
	struct cf *     pool;
	int             outstanding;            /* a pool's buffers not released */
	const void *    vtable;                 /* a derived object's */
	void *          storage;
};

static long live;                       /* CF objects made here and not freed */
static void derived_finalize(struct cf *o);

#define kCFStringEncodingUTF8   0x08000100
#define kCVReturnSuccess        0
#define kCVAttachmentMode_ShouldPropagate 1
#define kCMAttachmentMode_ShouldPropagate 1
#define QOS_CLASS_USER_INTERACTIVE 0x21
#define CF_CONSTANT(t, ...)     { .magic = CF_MAGIC, .type = (t), .refs = -1, __VA_ARGS__ }
#define CFSTR(s)                ({ static struct cf cfstr_ = CF_CONSTANT(T_STRING, .string = (s)); (CFStringRef)&cfstr_; })

static struct cf true_object = CF_CONSTANT(T_BOOLEAN, .number = 1), false_object = CF_CONSTANT(T_BOOLEAN, .number = 0);
#define kCFBooleanTrue          ((CFBooleanRef)&true_object)
#define kCFBooleanFalse         ((CFBooleanRef)&false_object)
static const int kCFTypeArrayCallBacks, kCFTypeDictionaryKeyCallBacks, kCFTypeDictionaryValueCallBacks, kCFTypeSetCallBacks;
static struct cf cv_keys[6] = {
	CF_CONSTANT(T_STRING, .string = "ColorPrimaries"), CF_CONSTANT(T_STRING, .string = "YCbCrMatrix"),
	CF_CONSTANT(T_STRING, .string = "TransferFunction"), CF_CONSTANT(T_STRING, .string = "ITU_R_709_2"),
	CF_CONSTANT(T_STRING, .string = "ITU_R_601_4"), CF_CONSTANT(T_STRING, .string = "ITU_R_709_2 (transfer)"),
};
#define kCVImageBufferColorPrimariesKey                 ((CFStringRef)&cv_keys[0])
#define kCVImageBufferYCbCrMatrixKey                    ((CFStringRef)&cv_keys[1])
#define kCVImageBufferTransferFunctionKey               ((CFStringRef)&cv_keys[2])
#define kCVImageBufferColorPrimaries_ITU_R_709_2        ((CFStringRef)&cv_keys[3])
#define kCVImageBufferYCbCrMatrix_ITU_R_601_4           ((CFStringRef)&cv_keys[4])
#define kCVImageBufferTransferFunction_ITU_R_709_2      ((CFStringRef)&cv_keys[5])

static struct cf *
cf_new(CFTypeID type)
{
	struct cf *o = calloc(1, sizeof(*o));

	o->magic = CF_MAGIC;
	o->type = type;
	o->refs = 1;
	__atomic_add_fetch(&live, 1, __ATOMIC_RELAXED);
	return o;
}

static bool
counted_object(CFTypeRef p)
{
	return p != NULL && ((const struct cf *)p)->magic == CF_MAGIC;
}

static CFTypeRef
CFRetain(CFTypeRef p)
{
	struct cf *o = (struct cf *)p;

	if (p == NULL) {
		printf("FAIL: CFRetain(NULL)\n");
		abort();
	}
	if (counted_object(p) && o->refs >= 0) {
		__atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
	}
	return p;
}

static void
CFRelease(CFTypeRef p)
{
	struct cf *o = (struct cf *)p;

	if (p == NULL) {
		printf("FAIL: CFRelease(NULL)\n");
		abort();
	}
	if (!counted_object(p) || o->refs < 0 || __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) > 0) {
		return;
	}
	if (o->type == T_DERIVED) {
		derived_finalize(o);
		free(o->storage);
	}
	for (CFIndex i = 0; i < o->count; i++) {
		if (o->keys != NULL) {
			CFRelease(o->keys[i]);
		}
		if (o->values != NULL) {
			CFRelease(o->values[i]);
		}
	}
	free(o->keys);
	free(o->values);
	free((void *)(o->type == T_STRING ? o->string : NULL));
	if (o->attachments != NULL) {
		CFRelease(o->attachments);
	}
	if (o->type == T_PIXEL_BUFFER) {
		free(o->plane[0]);
		free(o->plane[1]);
		__atomic_sub_fetch(&o->pool->outstanding, 1, __ATOMIC_RELAXED);
		CFRelease(o->pool);
	}
	o->magic = 0;
	free(o);
	__atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED);
}

static CFTypeID CFStringGetTypeID(void) { return T_STRING; }
static CFTypeID CFNumberGetTypeID(void) { return T_NUMBER; }
static CFTypeID CFBooleanGetTypeID(void) { return T_BOOLEAN; }
static CFTypeID CFDictionaryGetTypeID(void) { return T_DICTIONARY; }
static CFTypeID CFArrayGetTypeID(void) { return T_ARRAY; }
static CFTypeID CVPixelBufferPoolGetTypeID(void) { return T_POOL; }
static CFTypeID CFGetTypeID(CFTypeRef p) { return counted_object(p) ? ((const struct cf *)p)->type : 0; }
static Boolean CFBooleanGetValue(CFBooleanRef b) { return b->number != 0; }

static Boolean
CFEqual(CFTypeRef a, CFTypeRef b)
{
	const struct cf *x = a, *y = b;

	if (a == b) {
		return true;
	}
	if (!counted_object(a) || !counted_object(b) || x->type != y->type) {
		return false;
	}
	return x->type == T_STRING ? strcmp(x->string, y->string) == 0 : x->type == T_NUMBER ? x->number == y->number : false;
}

static CFStringRef
string_take(char *s)
{
	struct cf *o = cf_new(T_STRING);

	o->string = s;
	return o;
}

static CFStringRef
CFStringCreateWithCString(CFAllocatorRef allocator, const char *s, CFStringEncoding encoding)
{
	(void)allocator;
	(void)encoding;
	return string_take(strdup(s));
}

static Boolean
CFStringGetCString(CFStringRef s, char *out, CFIndex size, CFStringEncoding encoding)
{
	(void)encoding;
	if (!counted_object(s) || s->type != T_STRING || strlen(s->string) >= (size_t)size) {
		return false;
	}
	strcpy(out, s->string);
	return true;
}

static CFStringRef
CFCopyDescription(CFTypeRef p)
{
	const struct cf *o = p;
	char text[64];

	if (counted_object(p) && o->type == T_STRING) {
		return CFStringCreateWithCString(NULL, o->string, 0);
	}
	snprintf(text, sizeof(text), counted_object(p) && o->type == T_NUMBER ? "%g" : "<object>", o->number);
	return CFStringCreateWithCString(NULL, text, 0);
}

/* (as the file uses it: %s and %@ only) */
static CFStringRef
CFStringCreateWithFormat(CFAllocatorRef allocator, CFDictionaryRef options, CFStringRef format, ...)
{
	char out[512] = "";
	size_t n = 0;
	va_list ap;

	(void)allocator;
	(void)options;
	va_start(ap, format);
	for (const char *f = format->string; *f != '\0' && n + 1 < sizeof(out); f++) {
		const char *piece = NULL;
		char one[2] = { *f, 0 };

		if (f[0] == '%' && f[1] == 's') {
			piece = va_arg(ap, const char *);
			f++;
		} else if (f[0] == '%' && f[1] == '@') {
			CFTypeRef o = va_arg(ap, CFTypeRef);

			piece = counted_object(o) && ((const struct cf *)o)->type == T_STRING ? ((const struct cf *)o)->string : "<object>";
			f++;
		} else {
			piece = one;
		}
		n += (size_t)snprintf(out + n, sizeof(out) - n, "%s", piece);
		n = n >= sizeof(out) ? sizeof(out) - 1 : n;
	}
	va_end(ap);
	return string_take(strdup(out));
}

static CFNumberRef
CFNumberCreate(CFAllocatorRef allocator, CFNumberType type, const void *value)
{
	struct cf *o = cf_new(T_NUMBER);

	(void)allocator;
	o->number = type == kCFNumberSInt64Type ? (double)*(const int64_t *)value :
	    type == kCFNumberFloatType ? *(const float *)value : *(const double *)value;
	o->integer = type == kCFNumberSInt64Type ? *(const int64_t *)value : fabs(o->number) < 9e18 ? (int64_t)o->number : 0;
	return o;
}

static Boolean
CFNumberGetValue(CFNumberRef n, CFNumberType type, void *out)
{
	if (type == kCFNumberSInt64Type) {
		*(int64_t *)out = n->integer;
	} else if (type == kCFNumberFloatType) {
		*(float *)out = (float)n->number;
	} else {
		*(double *)out = n->number;
	}
	return true;
}

static void
put_pair(struct cf *d, const void *key, const void *value)
{
	if (d->count == d->capacity) {
		d->capacity = d->capacity * 2 + 8;
		d->keys = realloc(d->keys, sizeof(void *) * (size_t)d->capacity);
		d->values = d->type == T_SET ? NULL : realloc(d->values, sizeof(void *) * (size_t)d->capacity);
	}
	d->keys[d->count] = CFRetain(key);
	if (d->values != NULL) {
		d->values[d->count] = CFRetain(value);
	}
	d->count++;
}

static CFIndex
find_key(CFDictionaryRef d, const void *key)
{
	for (CFIndex i = 0; d != NULL && i < d->count; i++) {
		if (CFEqual(d->keys[i], key)) {
			return i;
		}
	}
	return -1;
}

static CFMutableDictionaryRef
CFDictionaryCreateMutable(CFAllocatorRef allocator, CFIndex capacity, const void *key_callbacks, const void *value_callbacks)
{
	(void)allocator;
	(void)capacity;
	(void)key_callbacks;
	(void)value_callbacks;
	return cf_new(T_DICTIONARY);
}

static CFDictionaryRef
CFDictionaryCreate(CFAllocatorRef allocator, const void **keys, const void **values, CFIndex n, const void *key_callbacks,
    const void *value_callbacks)
{
	struct cf *d = CFDictionaryCreateMutable(allocator, n, key_callbacks, value_callbacks);

	for (CFIndex i = 0; i < n; i++) {
		put_pair(d, keys[i], values[i]);
	}
	return d;
}

static CFMutableDictionaryRef
CFDictionaryCreateMutableCopy(CFAllocatorRef allocator, CFIndex capacity, CFDictionaryRef from)
{
	struct cf *d = CFDictionaryCreateMutable(allocator, capacity, NULL, NULL);

	for (CFIndex i = 0; i < from->count; i++) {
		put_pair(d, from->keys[i], from->values[i]);
	}
	return d;
}

static const void *
CFDictionaryGetValue(CFDictionaryRef d, const void *key)
{
	CFIndex i = find_key(d, key);

	return i >= 0 ? d->values[i] : NULL;
}

static Boolean CFDictionaryContainsKey(CFDictionaryRef d, const void *key) { return find_key(d, key) >= 0; }
static CFIndex CFDictionaryGetCount(CFDictionaryRef d) { return d->count; }

static void
CFDictionarySetValue(CFMutableDictionaryRef d, const void *key, const void *value)
{
	CFIndex i = find_key(d, key);

	if (i < 0) {
		put_pair(d, key, value);
		return;
	}
	CFRetain(value);
	CFRelease(d->values[i]);
	d->values[i] = value;
}

static void
CFDictionaryGetKeysAndValues(CFDictionaryRef d, const void **keys, const void **values)
{
	for (CFIndex i = 0; i < d->count; i++) {
		keys[i] = d->keys[i];
		values[i] = d->values[i];
	}
}

static void
CFDictionaryApplyFunction(CFDictionaryRef d, CFDictionaryApplierFunction applier, void *context)
{
	for (CFIndex i = 0; i < d->count; i++) {
		applier(d->keys[i], d->values[i], context);
	}
}

static CFArrayRef
CFArrayCreate(CFAllocatorRef allocator, const void **values, CFIndex n, const void *callbacks)
{
	struct cf *a = cf_new(T_ARRAY);

	(void)allocator;
	(void)callbacks;
	a->values = calloc((size_t)n + 1, sizeof(void *));
	for (CFIndex i = 0; i < n; i++) {
		a->values[i] = CFRetain(values[i]);
	}
	a->count = n;
	return a;
}

static CFIndex CFArrayGetCount(CFArrayRef a) { return a->count; }
static const void *CFArrayGetValueAtIndex(CFArrayRef a, CFIndex i) { return a->values[i]; }

static CFMutableSetRef
CFSetCreateMutable(CFAllocatorRef allocator, CFIndex capacity, const void *callbacks)
{
	(void)allocator;
	(void)capacity;
	(void)callbacks;
	return cf_new(T_SET);
}

static Boolean CFSetContainsValue(CFSetRef s, const void *value) { return find_key(s, value) >= 0; }

static void
CFSetAddValue(CFMutableSetRef s, const void *value)
{
	if (find_key(s, value) < 0) {
		put_pair(s, value, NULL);
	}
}

static struct cf host_clock = CF_CONSTANT(T_CLOCK);

static CMClockRef CMClockGetHostTimeClock(void) { return &host_clock; }
static CMTime CMClockMakeHostTimeFromSystemUnits(uint64_t t) { return (CMTime){ (int64_t)t, 1000000000, 1, 0 }; }

static CFDictionaryRef
CMTimeCopyAsDictionary(CMTime t, CFAllocatorRef allocator)
{
	struct cf *d = CFDictionaryCreateMutable(allocator, 0, NULL, NULL);
	CFNumberRef v = CFNumberCreate(NULL, kCFNumberSInt64Type, &t.value);

	CFDictionarySetValue(d, CFSTR("value"), v);
	CFRelease(v);
	return d;
}

static OSStatus
CMVideoFormatDescriptionCreate(CFAllocatorRef allocator, uint32_t codec, int32_t width, int32_t height, CFDictionaryRef extensions,
    CMVideoFormatDescriptionRef *out)
{
	(void)allocator;
	(void)codec;
	(void)width;
	(void)height;
	(void)extensions;
	*out = cf_new(T_OTHER);
	return 0;
}

static CVImageBufferRef CMSampleBufferGetImageBuffer(CMSampleBufferRef sbuf) { return sbuf->pool; }

static CFTypeRef
CMGetAttachment(CFTypeRef target, CFStringRef key, CMAttachmentMode *mode)
{
	(void)mode;
	return ((const struct cf *)target)->attachments != NULL ? CFDictionaryGetValue(((const struct cf *)target)->attachments, key) : NULL;
}

static void
CMSetAttachment(CFTypeRef target, CFStringRef key, CFTypeRef value, CMAttachmentMode mode)
{
	struct cf *o = (struct cf *)target;

	(void)mode;
	if (o->attachments == NULL) {
		o->attachments = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
	}
	CFDictionarySetValue(o->attachments, key, value);
}

#define CVBufferGetAttachment   CMGetAttachment
#define CVBufferSetAttachment   CMSetAttachment

static CVPixelBufferPoolRef
pool_make(uint32_t width, uint32_t height, uint32_t format)
{
	struct cf *p = cf_new(T_POOL);

	p->width = width;
	p->height = height;
	p->format = format;
	return p;
}

static CVReturn
CVPixelBufferPoolCreatePixelBuffer(CFAllocatorRef allocator, CVPixelBufferPoolRef pool, CVPixelBufferRef *out)
{
	struct cf *b = cf_new(T_PIXEL_BUFFER);
	bool bgra = pool->format == 0x42475241;

	(void)allocator;
	b->width = pool->width;
	b->height = pool->height;
	b->format = pool->format;
	b->row_bytes[0] = bgra ? 4 * (size_t)pool->width : pool->width;
	b->plane_height[0] = pool->height;
	b->row_bytes[1] = bgra ? 0 : ((size_t)pool->width + 1) / 2 * 2;
	b->plane_height[1] = bgra ? 0 : ((size_t)pool->height + 1) / 2;
	b->plane[0] = malloc(b->row_bytes[0] * b->plane_height[0]);
	b->plane[1] = bgra ? NULL : malloc(b->row_bytes[1] * b->plane_height[1]);
	b->pool = (struct cf *)CFRetain(pool);
	__atomic_add_fetch(&pool->outstanding, 1, __ATOMIC_RELAXED);
	*out = b;
	return kCVReturnSuccess;
}

static void CVPixelBufferRelease(CVPixelBufferRef b) { if (b != NULL) CFRelease(b); }
static uint32_t CVPixelBufferGetPixelFormatType(CVPixelBufferRef b) { return b->format; }
static size_t CVPixelBufferGetWidth(CVPixelBufferRef b) { return b->width; }
static size_t CVPixelBufferGetHeight(CVPixelBufferRef b) { return b->height; }
static size_t CVPixelBufferGetPlaneCount(CVPixelBufferRef b) { return b->plane[1] != NULL ? 2 : 0; }
static void *CVPixelBufferGetBaseAddress(CVPixelBufferRef b) { return b->plane[0]; }
static size_t CVPixelBufferGetBytesPerRow(CVPixelBufferRef b) { return b->row_bytes[0]; }
static void *CVPixelBufferGetBaseAddressOfPlane(CVPixelBufferRef b, size_t i) { return i < 2 ? b->plane[i] : NULL; }
static size_t CVPixelBufferGetBytesPerRowOfPlane(CVPixelBufferRef b, size_t i) { return i < 2 ? b->row_bytes[i] : 0; }
static size_t CVPixelBufferGetHeightOfPlane(CVPixelBufferRef b, size_t i) { return i < 2 ? b->plane_height[i] : 0; }
static CVReturn CVPixelBufferLockBaseAddress(CVPixelBufferRef b, CVPixelBufferLockFlags f) { (void)b; (void)f; return 0; }
static CVReturn CVPixelBufferUnlockBaseAddress(CVPixelBufferRef b, CVPixelBufferLockFlags f) { (void)b; (void)f; return 0; }

/* ---- libdispatch and mach ------------------------------------------------------------------------------------- */

static dispatch_queue_t dispatch_get_global_queue(long qos, unsigned long flags) { (void)qos; (void)flags; return NULL; }

static void
dispatch_apply_f(size_t n, dispatch_queue_t queue, void *context, void (*work)(void *, size_t))
{
	(void)queue;
	for (size_t i = 0; i < n; i++) {
		work(context, i);
	}
}

static uint64_t
mach_absolute_time(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int mach_timebase_info(mach_timebase_info_data_t *tb) { tb->numer = tb->denom = 1; return 0; }

/* ---- the kernel's /dev/hakios-camera, the test's ----------------------------------------------------------------- */

#include "../userland/userland/src/board/camera.h"

#define CAMERA_FD       900             /* (no file of the process's has this number) */
#define WIDTH           64
#define HEIGHT          48
#define STRIDE          ((WIDTH * 10 / 8 + 15) & ~15u)

static struct {
	pthread_mutex_t lock;
	unsigned        tries, opens, closes, reads, sensor_commands;  /* (tries: opens, failed ones too) */
	uint32_t        sensor;         /* whose frames it gives */
	uint64_t        sequence;
	uint64_t        fault_after;    /* frames given before the camera has a fault (0: never) */
	bool            broken;         /* EIO, for the rest of the run, as the kernel's give_up() */
} kernel = { .lock = PTHREAD_MUTEX_INITIALIZER };
static const char *tuning_path;         /* what /hakios/fw/camera-<chip>.isp is here (NULL: none) */

static int
test_open(const char *path, int flags, ...)
{
	if (strcmp(path, HAKIOS_CAMERA_DEVICE) == 0) {
		int fd = CAMERA_FD;

		pthread_mutex_lock(&kernel.lock);
		kernel.tries++;
		if (kernel.broken) {
			errno = EIO;
			fd = -1;
		} else {
			kernel.opens++;
		}
		pthread_mutex_unlock(&kernel.lock);
		return fd;
	}
	if (strncmp(path, "/hakios/fw/camera-", 18) == 0 && tuning_path != NULL) {
		return open(tuning_path, flags);
	}
	errno = ENOENT;
	return -1;
}

static ssize_t
test_read(int fd, void *buffer, size_t bytes)
{
	struct hakios_camera_frame f = { .header_bytes = sizeof(f), .width = WIDTH, .height = HEIGHT, .stride = STRIDE, .format = 1,
		.black = 64, .exposure_lines = 1000, .line_ns = 10000, .gain = 32, .gain_unity = 32, .gain_max = 512, .frame_lines = 3300,
		.frame_lines_min = 3300, .frame_lines_max = 60000, .margin = 8, .focus = HAKIOS_CAMERA_NO_FOCUS, .chip = "S5KHM2SD",
		.darwin_back = 0, .darwin_front = 1 };
	size_t n;

	if (fd != CAMERA_FD) {
		return read(fd, buffer, bytes);
	}
	usleep(500);                    /* (a frame's time, short) */
	pthread_mutex_lock(&kernel.lock);
	kernel.reads++;
	if (!kernel.broken && kernel.fault_after != 0 && kernel.sequence >= kernel.fault_after) {
		kernel.broken = true;
	}
	if (kernel.broken) {
		pthread_mutex_unlock(&kernel.lock);
		errno = EIO;
		return -1;
	}
	memcpy(f.magic, HAKIOS_CAMERA_MAGIC, 8);
	f.sequence = ++kernel.sequence;
	f.time = f.sequence * 33000000;
	f.sensor = kernel.sensor;
	pthread_mutex_unlock(&kernel.lock);
	n = bytes < sizeof(f) ? bytes : sizeof(f);
	memcpy(buffer, &f, n);
	if (bytes > sizeof(f)) {
		size_t data = bytes - sizeof(f) < (size_t)STRIDE * HEIGHT ? bytes - sizeof(f) : (size_t)STRIDE * HEIGHT;

		memset((uint8_t *)buffer + sizeof(f), 0x50, data);
		n += data;
	}
	return (ssize_t)n;
}

static ssize_t
test_write(int fd, const void *buffer, size_t bytes)
{
	const struct hakios_camera_command *c = buffer;

	if (fd != CAMERA_FD) {
		return write(fd, buffer, bytes);
	}
	if (bytes >= sizeof(*c) && c->command == HAKIOS_CAMERA_SENSOR) {
		pthread_mutex_lock(&kernel.lock);
		kernel.sensor_commands++;
		kernel.sensor = c->a;
		pthread_mutex_unlock(&kernel.lock);
	}
	return (ssize_t)bytes;
}

static int
test_close(int fd)
{
	if (fd != CAMERA_FD) {
		return close(fd);
	}
	pthread_mutex_lock(&kernel.lock);
	kernel.closes++;
	pthread_mutex_unlock(&kernel.lock);
	return 0;
}

static void *test_dlsym(void *handle, const char *name);

/* ---- the files ------------------------------------------------------------------------------------------------- */

#define malloc(n)                       counted_malloc(n)
#define calloc(n, s)                    counted_calloc(n, s)
#define free(p)                         counted_free(p)
#define open                            test_open
#define read                            test_read
#define write                           test_write
#define close                           test_close
#define dlsym                           test_dlsym
#define pthread_setname_np(name)        pthread_setname_np(pthread_self(), name)
/* (as cameracaptured's library builds it, with iOS 27's formats; Darwin's uint64_t is an unsigned long long, and the
 * file's initializers leave the fields at the end to be zero) */
#define HAKIOS_IOS27
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#include "../components/camera/src/camera_isp.c"
#include "../components/camera/src/camera_device.c"
#pragma clang diagnostic pop
#undef malloc
#undef calloc
#undef free
#undef open
#undef read
#undef write
#undef close
#undef dlsym

/* CoreMedia's derived objects, as the file lays out their tables */
static OSStatus
test_derived_create(CFAllocatorRef allocator, const struct vtable *vtable, CMBaseClassID class_id, CMBaseObjectRef *out)
{
	struct cf *o = cf_new(T_DERIVED);

	(void)allocator;
	(void)class_id;
	o->vtable = vtable;
	o->storage = calloc(1, vtable->base->storage_size);
	*out = (CMBaseObjectRef)o;
	return 0;
}

static void *test_derived_storage(CMBaseObjectRef o) { return ((struct cf *)o)->storage; }

static void
derived_finalize(struct cf *o)
{
	const struct vtable *v = o->vtable;

	v->base->finalize((CMBaseObjectRef)o);
}

static int device_class_tag, stream_class_tag;
static CMBaseClassID test_device_class(void) { return &device_class_tag; }
static CMBaseClassID test_stream_class(void) { return &stream_class_tag; }

/* the names the file looks up: CoreMedia's and CMCapture's functions and keys */
static CFStringRef dlsym_strings[MD_KEYS + 3];

static void *
test_dlsym(void *handle, const char *name)
{
	static const char *const ports[3] = { "kFigCapturePortType_BackFacingCamera", "kFigCapturePortType_FrontFacingCamera",
		"kFigCaptureSampleBufferAttachmentKey_MetadataDictionary" };

	(void)handle;
	if (strcmp(name, "CMDerivedObjectCreate") == 0) {
		return (void *)test_derived_create;
	}
	if (strcmp(name, "CMBaseObjectGetDerivedStorage") == 0) {
		return (void *)test_derived_storage;
	}
	if (strcmp(name, "FigCaptureDeviceGetClassID") == 0) {
		return (void *)test_device_class;
	}
	if (strcmp(name, "FigCaptureStreamGetClassID") == 0) {
		return (void *)test_stream_class;
	}
	for (int i = 0; i < MD_KEYS + 3; i++) {
		const char *known = i < 3 ? ports[i] : md_names[i - 3];

		if (strcmp(name, known) == 0) {
			if (dlsym_strings[i] == NULL) {
				dlsym_strings[i] = CFStringCreateWithCString(NULL, known, 0);
			}
			return &dlsym_strings[i];
		}
	}
	return NULL;
}

/* ---- the capture graph's side ---------------------------------------------------------------------------------- */

/* a block as clang lays one out, made by hand: ^(CVPixelBufferRef, CMTime), counting the pictures it is given */
static unsigned pictures;
static long live_at[2];                 /* CF objects there are at the 10th picture and at the 40th */

static void
picture_given(void *block, CVPixelBufferRef pb, CMTime t)
{
	(void)block;
	(void)t;
	CHECK(pb != NULL && pb->width == WIDTH / 2 && pb->height == HEIGHT / 2, "a picture of the pool's size");
	pictures++;
	if (pictures == 10 || pictures == 40) {
		live_at[pictures == 40] = __atomic_load_n(&live, __ATOMIC_RELAXED);
	}
}

static struct {
	unsigned long reserved, size;
} handler_descriptor = { 0, 40 };
static struct {
	void *isa;
	int flags, reserved;
	void (*invoke)(void *, CVPixelBufferRef, CMTime);
	const void *descriptor;
} handler_block = { NULL, 1 << 28, 0, picture_given, &handler_descriptor };

static CMBaseObjectRef device_made, stream_back;

static void
device_make(void)
{
	CMBaseObjectRef d = NULL;
	CFArrayRef streams = NULL;

	CHECK(hakios_camera_device_create(NULL, NULL, &d) == 0 && d != NULL, "the device is made");
	CHECK(device_copy_property(d, CFSTR("StreamArray"), NULL, &streams) == 0 && CFArrayGetCount(streams) == 2, "two streams");
	device_made = d;
	stream_back = (CMBaseObjectRef)CFArrayGetValueAtIndex(streams, 0);
	CFRelease(streams);
}

static void
set_one(CMBaseObjectRef st, const char *key, const char *output, CFTypeRef value)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key, 0), o = CFStringCreateWithCString(NULL, output, 0);
	CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)&o, &value, 1, NULL, NULL);

	CHECK(stream_set_property(st, k, d) == 0, "%s is taken", key);
	CFRelease(d);
	CFRelease(o);
	CFRelease(k);
}

/* the graph's preview output: its pool (half the sensor's size each way, 420f), its handler, on */
static void
preview_configure(CMBaseObjectRef st)
{
	CVPixelBufferPoolRef pool = pool_make(WIDTH / 2, HEIGHT / 2, PF_420F);
	CFDictionaryRef config = CFDictionaryCreate(NULL, (const void *[]){ CFSTR("BufferPool") }, (const void *[]){ pool }, 1, NULL, NULL);
	CFDictionaryRef handler = CFDictionaryCreate(NULL, (const void *[]){ CFSTR("PixelBufferHandler") },
	    (const void *[]){ &handler_block }, 1, NULL, NULL);

	set_one(st, "VideoOutputConfigurations", "PrimaryScaler", config);
	set_one(st, "VideoOutputHandlers", "PrimaryScaler", handler);
	set_one(st, "VideoOutputsEnabled", "PrimaryScaler", kCFBooleanTrue);
	CFRelease(handler);
	CFRelease(config);
	CFRelease(pool);
}

static bool
wait_for(bool (*done)(void), int ms)
{
	for (int i = 0; i < ms && !done(); i++) {
		usleep(1000);
	}
	return done();
}

static unsigned want_pictures;
static bool enough_pictures(void) { return pictures >= want_pictures; }
static bool worker_ended(void) { return __atomic_load_n(&stream_of(stream_back)->worker_done, __ATOMIC_ACQUIRE); }

static void
device_end(void)
{
	CFRelease(device_made);
	CHECK(kernel.opens == kernel.closes, "each open of the camera closed: %u opens, %u closes", kernel.opens, kernel.closes);
}

/* ---- the scenarios ------------------------------------------------------------------------------------------------ */

/* Frames to the preview, as the graph starts and stops the stream: no CF object left behind by a frame, nothing kept. */
static void
scenario_frames(void)
{
	device_make();
	preview_configure(stream_back);
	CHECK(stream_start(stream_back) == 0, "the stream starts");
	want_pictures = 45;
	CHECK(wait_for(enough_pictures, 20000), "pictures given: %u", pictures);
	CHECK(stream_stop(stream_back) == 0, "the stream stops");
	CHECK(live_at[0] != 0 && live_at[0] == live_at[1], "as many CF objects at the 40th picture as at the 10th: %ld, %ld",
	    live_at[0], live_at[1]);
	CHECK(kernel.opens == 1 && kernel.closes == 1, "the camera opened once and closed: %u, %u", kernel.opens, kernel.closes);
	device_end();
	CHECK(outstanding == 0, "nothing the files allocated is left: %ld", outstanding);
}

/* writes a camera tuning of the smallest kind (one light, one colour temperature: components/camera/tools/camtuning.py) */
static void
tuning_write(const char *path)
{
	FILE *f = fopen(path, "wb");
	uint32_t one = 1, zero = 0;
	float range[2] = { 0, 1000 }, cct[2] = { 2000, 8000 }, identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, flat = 1;

	fwrite("RWCISP02", 1, 8, f);
	for (int set = 0; set < 2; set++) {
		fwrite(&one, 4, 1, f);
		fwrite(range, 4, 2, f);
		fwrite(&one, 4, 1, f);
		fwrite(cct, 4, 2, f);
		if (set == 0) {
			fwrite(identity, 4, 9, f);
		} else {
			for (int i = 0; i < 4 * LSC_W * LSC_H; i++) {
				fwrite(&flat, 4, 1, f);
			}
		}
	}
	fwrite(&zero, 4, 1, f);
	fclose(f);
}

/*
 * The camera started and stopped again and again -- Camera.app opened and closed, a turn to the other camera and back --
 * with a tuning for its sensor: each worker loads the tuning, and nothing of it is left once the worker has ended.
 */
static void
scenario_restarts_leave_nothing(void)
{
	char path[512];
	int fd;

	snprintf(path, sizeof(path), "%s/camera_device_test.XXXXXX", getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	fd = mkstemp(path);

	CHECK(fd >= 0, "a file for the tuning");
	close(fd);
	tuning_write(path);
	tuning_path = path;
	device_make();
	preview_configure(stream_back);
	for (int i = 0; i < 4; i++) {
		pictures = 0;
		want_pictures = 3;
		CHECK(stream_start(stream_back) == 0, "the stream starts (%d)", i);
		CHECK(wait_for(enough_pictures, 20000), "pictures given (%d): %u", i, pictures);
		CHECK(stream_stop(stream_back) == 0, "the stream stops (%d)", i);
		CHECK(outstanding == 0, "nothing of the worker's left once it has ended (start %d): %ld allocations", i + 1,
		    outstanding);
	}
	device_end();
	unlink(path);
}

/*
 * The kernel's camera has a fault while it streams: EIO at once, from then on, for the rest of the run (open() too). The
 * frames end; the graph's later reservations and starts make no worker that fails at once each time, and a start
 * says the stream did not start, not that it did.
 */
static void
scenario_fault_is_final(void)
{
	device_make();
	preview_configure(stream_back);
	kernel.fault_after = 20;
	CHECK(stream_start(stream_back) == 0, "the stream starts");
	CHECK(wait_for(worker_ended, 20000), "the frames end at the fault");
	CHECK(kernel.opens == 1, "the camera opened once: %u", kernel.opens);
	unsigned reads = kernel.reads;
	CFArrayRef both = NULL;

	device_copy_property(device_made, CFSTR("StreamArray"), NULL, &both);
	for (int i = 0; i < 50; i++) {
		device_request_streams(device_made, both, NULL);       /* (the reservations the graph makes again and again) */
		device_relinquish_streams(device_made, both, NULL);
	}
	CFRelease(both);
	CHECK(stream_start(stream_back) != 0, "a start after the fault fails");
	usleep(20000);
	CHECK(kernel.tries == 1 && kernel.reads == reads, "no worker made again after the fault: the camera opened %u times, "
	    "read %u times more", kernel.tries, kernel.reads - reads);
	CHECK(stream_stop(stream_back) == 0, "the stream stops");
	device_end();
}

/*
 * What the graph sets the stream's zoom and focus window to, as floating-point numbers it may get wrong: a zoom that is
 * not a number is no zoom, never a NaN in the crop the pictures are made of; a window not a number, or wider than the
 * frame, is a window within the frame (isp_sharpness() takes fractions of it), never a NaN or a negative start.
 */
static void
scenario_floats_from_the_graph(void)
{
	struct stream *s;
	double nan_value = NAN;
	CFNumberRef nan_number = CFNumberCreate(NULL, kCFNumberDoubleType, &nan_value);
	CFNumberRef zoom = NULL;
	double z = 0;

	device_make();
	s = stream_of(stream_back);
	CHECK(stream_set_property(stream_back, CFSTR("ZoomFactor"), nan_number) == 0, "a zoom taken");
	CHECK(stream_copy_property(stream_back, CFSTR("ZoomFactor"), NULL, &zoom) == 0, "the zoom answered");
	CFNumberGetValue(zoom, kCFNumberDoubleType, &z);
	CFRelease(zoom);
	CHECK(z >= 1 && z <= MAX_ZOOM, "a zoom of NaN is no zoom: %g", z);
	CHECK(s->zoom >= 1 && s->zoom <= MAX_ZOOM, "the pictures' zoom: %g", s->zoom);

	static const double windows[][4] = {
		{ NAN, NAN, 200, 200 }, { 100, 100, 5000, 5000 }, { -1e9, 1e9, 3000, 50 }, { INFINITY, -INFINITY, 1e30, 1e30 },
		{ 400, 400, 200, 200 },
	};
	for (size_t i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
		const char *names[4] = { "hStart", "vStart", "width", "height" };
		CFMutableDictionaryRef w = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
		CFArrayRef a;

		for (int k = 0; k < 4; k++) {
			CFStringRef name = CFStringCreateWithCString(NULL, names[k], 0);
			CFNumberRef v = CFNumberCreate(NULL, kCFNumberDoubleType, &windows[i][k]);

			CFDictionarySetValue(w, name, v);
			CFRelease(v);
			CFRelease(name);
		}
		a = CFArrayCreate(NULL, (const void **)&w, 1, NULL);
		CHECK(stream_set_property(stream_back, CFSTR("AFWindowParams"), a) == 0, "a window taken");
		CFRelease(a);
		CFRelease(w);
		CHECK(s->af_window[0] >= 0 && s->af_window[1] >= 0 && s->af_window[2] <= 1 && s->af_window[3] <= 1 &&
		    s->af_window[0] < s->af_window[2] && s->af_window[1] < s->af_window[3],
		    "window %zu within the frame: %g %g %g %g", i, s->af_window[0], s->af_window[1], s->af_window[2], s->af_window[3]);
	}
	CHECK(fabsf(s->af_window[0] - 0.4f) < 1e-6f && fabsf(s->af_window[3] - 0.6f) < 1e-6f, "a window within the frame as asked");
	CFRelease(nan_number);
	device_end();
}

static const struct {
	const char *name;
	void (*run)(void);
} scenarios[] = {
	{ "frames", scenario_frames },
	{ "restarts_leave_nothing", scenario_restarts_leave_nothing },
	{ "fault_is_final", scenario_fault_is_final },
	{ "floats_from_the_graph", scenario_floats_from_the_graph },
};

int
main(int argc, char **argv)
{
	size_t n = sizeof(scenarios) / sizeof(scenarios[0]);

	if (argc < 2) {
		for (size_t i = 0; i < n; i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (size_t i = 0; i < n; i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			printf("camera_device_test %s: %d checks, %d failed\n", argv[1], checks, failures);
			return failures != 0;
		}
	}
	printf("camera_device_test: no scenario %s\n", argv[1]);
	return 2;
}
