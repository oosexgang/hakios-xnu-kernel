/*
 * Host tests of the touch screen and the buttons of Darwin 26's backboardd (userland/userland/src/inject26.c): its
 * part between "---- backboardd: the touch screen and the buttons" and "---- big thread stacks", which
 * workbench/run.sh cuts out of the file unchanged as inject26_input.c (the rest of the file is Objective-C
 * runtime calls and blocks, which have nothing to do with touches and which the host's compiler cannot build).
 * What it is given of CoreFoundation, IOKit's HID event calls, dispatch, notify and the Mach clock is this
 * file's: the events it makes are kept and looked at, and counted, so that one not let go is seen; and so are
 * its open(), read() and close() of /dev/hakios-touch, which play a script of samples and errors. The phone's
 * orientation (userland/userland/src/hid_orientation.h, included by the part), the accelerometer's samples
 * (hid_motion.h) and the gyroscope's rates (hid_gyro.h) are played the same way: what the kernel's sysctls say is a script, what a client asks of a
 * service is set through the service's own callbacks, and the calls the part looks up by name are this file's, or
 * missing.
 *
 *   backboard_input_test               the scenarios' names
 *   backboard_input_test SCENARIO      one scenario a process
 */
#define _DEFAULT_SOURCE
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
#include <unistd.h>

#include "../userland/userland/src/board/input.h"

/* ---- what the code needs of Darwin's headers --------------------------------------------------- */

#define __unused        __attribute__((unused))

typedef const void *CFTypeRef;
typedef const char *CFStringRef;        /* the characters themselves */
typedef const void *CFAllocatorRef, *CFDictionaryRef, *CFArrayRef, *CFNumberRef, *CFBooleanRef;
typedef void *CFMutableDictionaryRef, *CFMutableSetRef;
typedef long CFIndex;
typedef unsigned long CFTypeID;
typedef unsigned char Boolean;
#define CFSTR(s)                ((CFStringRef)(s))
#define kCFNumberIntType        9
#define kCFNumberSInt64Type     4
#define kCFStringEncodingUTF8   0x08000100
static const int kCFTypeSetCallBacks, kCFTypeDictionaryKeyCallBacks, kCFTypeDictionaryValueCallBacks, kCFTypeArrayCallBacks;
static const char cf_true[] = "true", cf_object[] = "a CoreFoundation object";
#define kCFBooleanTrue          ((CFBooleanRef)cf_true)

/* numbers that hold their value (a few at a time: the oldest is reused), so that what is set and asked can be read */
static struct test_number {
	long long value;
} test_numbers[256];
static unsigned test_number_count;

static bool
is_number(const void *o)
{
	return (uintptr_t)o >= (uintptr_t)test_numbers && (uintptr_t)o < (uintptr_t)(test_numbers + 256);
}

static CFNumberRef
CFNumberCreate(CFAllocatorRef a, int type, const void *value)
{
	struct test_number *n = &test_numbers[test_number_count++ % 256];

	(void)a;
	n->value = type == kCFNumberSInt64Type ? *(const long long *)value : *(const int *)value;
	return n;
}
static CFMutableDictionaryRef
CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex n, const void *k, const void *v)
{
	(void)a; (void)n; (void)k; (void)v;
	return (CFMutableDictionaryRef)cf_object;
}
/* the usage page and usage the part last gave a service, which IOHIDVirtualServiceClientCreateWithCallbacks() keeps */
static long long given_page = -1, given_usage = -1;
static void
CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v)
{
	(void)d;
	if (is_number(v) && strcmp(k, "PrimaryUsagePage") == 0) {
		given_page = ((const struct test_number *)v)->value;
	} else if (is_number(v) && strcmp(k, "PrimaryUsage") == 0) {
		given_usage = ((const struct test_number *)v)->value;
	}
}
static const void *CFDictionaryGetValue(CFDictionaryRef d, const void *k) { (void)d; (void)k; return NULL; }
static CFArrayRef CFArrayCreate(CFAllocatorRef a, const void **v, CFIndex n, const void *c) { (void)a; (void)v; (void)n; (void)c; return cf_object; }
static CFStringRef CFStringCreateWithCString(CFAllocatorRef a, const char *s, int e) { (void)a; (void)e; return s; }
static Boolean
CFStringGetCString(CFStringRef s, char *buffer, CFIndex size, int e)
{
	(void)e;
	snprintf(buffer, (size_t)size, "%s", s);
	return true;
}
static CFMutableSetRef CFSetCreateMutable(CFAllocatorRef a, CFIndex n, const void *c) { (void)a; (void)n; (void)c; return (CFMutableSetRef)cf_object; }
static Boolean CFSetContainsValue(CFMutableSetRef s, const void *v) { (void)s; (void)v; return true; }
static void CFSetAddValue(CFMutableSetRef s, const void *v) { (void)s; (void)v; }
static CFIndex CFSetGetCount(CFMutableSetRef s) { (void)s; return 0; }
static Boolean CFEqual(CFTypeRef a, CFTypeRef b) { return strcmp(a, b) == 0; }
static CFTypeID CFBooleanGetTypeID(void) { return 1; }
static CFTypeID CFNumberGetTypeID(void) { return 2; }
static CFTypeID CFGetTypeID(CFTypeRef o) { return is_number(o) ? CFNumberGetTypeID() : 0; }
static Boolean CFBooleanGetValue(CFBooleanRef b) { return b == kCFBooleanTrue; }
static Boolean
CFNumberGetValue(CFNumberRef n, int type, void *value)
{
	long long v = is_number(n) ? ((const struct test_number *)n)->value : 0;

	if (type == kCFNumberSInt64Type) {
		*(long long *)value = v;
	} else {
		*(int *)value = (int)v;
	}
	return true;
}
static CFStringRef CFCopyDescription(CFTypeRef o) { (void)o; return NULL; }
static CFTypeRef CFRetain(CFTypeRef o);
static void CFRelease(CFTypeRef o);

typedef void *dispatch_queue_t, *dispatch_semaphore_t;
static dispatch_queue_t dispatch_queue_create(const char *name, void *attr) { (void)name; (void)attr; return (void *)cf_object; }
static long dispatch_semaphore_signal(dispatch_semaphore_t s) { (void)s; return 0; }
static dispatch_semaphore_t dispatch_semaphore_create(long n) { (void)n; return (void *)cf_object; }

#define NOTIFY_TOKEN_INVALID    (-1)
#define NOTIFY_STATUS_OK        0
static int lock_state;                  /* what the side button was last said to be: 2 held, 1 released */
static uint32_t notify_register_check(const char *name, int *token) { (void)name; *token = 1; return NOTIFY_STATUS_OK; }
static uint32_t notify_set_state(int token, uint64_t state) { (void)token; lock_state = (int)state; return NOTIFY_STATUS_OK; }

typedef struct {
	uint32_t numer, denom;
} mach_timebase_info_data_t;
static uint64_t clock_now = 1000;
static uint64_t mach_absolute_time(void) { return clock_now++; }
static int mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = t->denom = 1; return 0; }

/*
 * The kernel's numbers, as hid_orientation.h, hid_motion.h and hid_gyro.h ask for them: hw.sensors.hid,
 * hw.sensors.orientation, hw.sensors.motion and hw.sensors.gyro each say the values of a script in turn, the last one for good; a name without a script is not
 * there (and kern.bootargs is not, as before). hw.sensors.sample says the texts of its own script the same way.
 */
struct number_script {
	const char *name;
	int values[64];
	unsigned count, next, asked;
};
static struct number_script kernel_hid = { .name = "hw.sensors.hid" }, kernel_orientation = { .name = "hw.sensors.orientation" },
	kernel_motion = { .name = "hw.sensors.motion" }, kernel_gyro = { .name = "hw.sensors.gyro" };
static struct {
	const char *texts[64];          /* NULL: a read that fails */
	long long asked_before[64];     /* before the look that reads this text, a client sets the "ReportInterval" ... */
	bool asks_before[64];           /* ... if this says so (when the look reads hw.sensors.motion, before what was asked) */
	unsigned count, next, asked;
} kernel_sample;
static void client_asks(long long interval);

static int
sysctlbyname(const char *n, void *o, size_t *s, void *i, size_t is)
{
	struct number_script *script = strcmp(n, kernel_hid.name) == 0 ? &kernel_hid :
	    strcmp(n, kernel_orientation.name) == 0 ? &kernel_orientation : strcmp(n, kernel_motion.name) == 0 ? &kernel_motion :
	    strcmp(n, kernel_gyro.name) == 0 ? &kernel_gyro : NULL;
	int value;

	(void)i; (void)is;
	if (strcmp(n, "hw.sensors.sample") == 0 && kernel_sample.count > 0 && o != NULL && s != NULL) {
		unsigned k = kernel_sample.next < kernel_sample.count ? kernel_sample.next++ : kernel_sample.count - 1;
		const char *text = kernel_sample.texts[k];

		kernel_sample.asked++;
		if (text == NULL || *s < strlen(text) + 1) {
			errno = text == NULL ? EIO : ENOMEM;
			return -1;
		}
		memcpy(o, text, strlen(text) + 1);
		*s = strlen(text) + 1;
		return 0;
	}
	if (script == &kernel_motion && kernel_sample.asks_before[kernel_sample.next]) {
		kernel_sample.asks_before[kernel_sample.next] = false;  /* (once: a look that reads no text comes again) */
		client_asks(kernel_sample.asked_before[kernel_sample.next]);
	}
	if (script == NULL || script->count == 0 || o == NULL || s == NULL || *s < sizeof(int)) {
		errno = ENOENT;
		return -1;
	}
	script->asked++;
	value = script->values[script->next < script->count ? script->next++ : script->count - 1];
	if (value == -99) {             /* (a read that fails) */
		errno = EIO;
		return -1;
	}
	memcpy(o, &value, sizeof(value));
	*s = sizeof(value);
	return 0;
}

/* IOKit's calls the part looks up by name (dlsym): this file's below, or none when a scenario says the calls are missing */
#define RTLD_DEFAULT    ((void *)0)
static bool hid_calls_missing;
static bool accelerometer_call_missing;        /* IOHIDEventCreateAccelerometerEvent alone */
static bool gyro_call_missing;                 /* IOHIDEventCreateGyroEvent alone */
static void *test_dlsym(void *handle, const char *name);
#define dlsym   test_dlsym

#define pthread_setname_np(name) ((void)(name))

static void
say(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}

static dispatch_semaphore_t input_services_made;

/* ---- the device: a script of what open() and read() give ---------------------------------------- */

static int opens[8];                    /* the errno of each open in turn, 0: it opens; past the last, ENOENT */
static unsigned open_scripted, open_count, opened, closed;
static struct {
	int error;                      /* 0: the sample */
	struct hakios_touch t;
} reads[32];                            /* past the last: EIO */
static unsigned read_count, reads_done;

static int
test_open(const char *path, int flags)
{
	(void)flags;
	if (strcmp(path, HAKIOS_TOUCH_DEVICE) != 0 || open_count >= open_scripted) {
		errno = ENOENT;
		return -1;
	}
	if (opens[open_count] != 0) {
		errno = opens[open_count++];
		return -1;
	}
	open_count++;
	opened++;
	return 7;
}

static ssize_t
test_read(int fd, void *buffer, size_t size)
{
	if (fd != 7 || opened == closed) {
		errno = EBADF;
		return -1;
	}
	if (reads_done >= read_count) {
		reads_done++;
		errno = EIO;
		return -1;
	}
	if (reads[reads_done].error != 0) {
		errno = reads[reads_done++].error;
		return -1;
	}
	if (size < sizeof(struct hakios_touch)) {
		errno = EINVAL;
		return -1;
	}
	memcpy(buffer, &reads[reads_done++].t, sizeof(struct hakios_touch));
	return sizeof(struct hakios_touch);
}

static int
test_close(int fd)
{
	(void)fd;
	closed++;
	return 0;
}

static unsigned test_sleep(unsigned s) { (void)s; return 0; }
static int test_usleep(unsigned us) { (void)us; return 0; }

/* ---- IOKit's HID events, kept ------------------------------------------------------------------ */

struct __IOHIDEventSystemClient {
	int unused;
};
struct __IOHIDServiceClient {
	int unused;
};
struct __IOHIDEvent {
	int references;
	bool keyboard;
	uint32_t type;                  /* IOHIDEventCreate()'s; 0 for the other makers' */
	uint32_t page;                  /* a vendor-defined event's, with its usage in `usage` and its bytes in `data` */
	uint8_t data[8];
	CFIndex length;
	struct {
		uint32_t field;
		CFIndex value;
	} fields[8];                    /* what IOHIDEventSetIntegerValue() set, in order */
	unsigned field_count;
	uint64_t time;
	uint32_t mask, index, usage;
	bool accelerometer;             /* IOHIDEventCreateAccelerometerEvent()'s, with its z in `z` */
	bool gyro;                      /* IOHIDEventCreateGyroEvent()'s, the same */
	double x, y, z;
	bool range, touch, down;
	unsigned children;
	struct __IOHIDEvent *child[16];
};

/*
 * The part under test: inject26.c's, or the same part of another backboardd library of the same shape, which a test
 * that includes this file names before it does (backboard_input16_test.c: inject.c's, as inject_input.c).
 */
#ifndef INPUT_PART
#define INPUT_PART      "inject26_input.c"
#define TEST_NAME       "backboard_input_test"
#endif

#define open    test_open
#define read    test_read
#define close   test_close
#define sleep   test_sleep
#define usleep  test_usleep
#include INPUT_PART
#undef dlsym
#undef open
#undef read
#undef close
#undef sleep
#undef usleep

static int live_events;                 /* made and not yet let go of */

static struct __IOHIDEvent *
event_new(void)
{
	struct __IOHIDEvent *e = calloc(1, sizeof(*e));

	e->references = 1;
	live_events++;
	return e;
}

static CFTypeRef
CFRetain(CFTypeRef o)
{
	return o;       /* (the code under test retains no event; nothing else it has is counted) */
}

static struct __IOHIDEvent *made_events[4096];
static unsigned made_count;

static void
CFRelease(CFTypeRef o)
{
	for (unsigned i = 0; i < made_count; i++) {
		struct __IOHIDEvent *e = made_events[i];

		if (e == o) {
			if (e->references <= 0) {
				fprintf(stderr, "FAIL: an event let go of once too often\n");
				exit(1);
			}
			if (--e->references == 0) {
				for (unsigned c = 0; c < e->children; c++) {
					CFRelease(e->child[c]);
				}
				live_events--;
			}
			return;
		}
	}
}

static struct __IOHIDEvent *
made(struct __IOHIDEvent *e)
{
	if (made_count == sizeof(made_events) / sizeof(made_events[0])) {
		abort();
	}
	made_events[made_count++] = e;
	return e;
}

static struct __IOHIDEventSystemClient the_client;
static struct __IOHIDServiceClient services[8];
static unsigned service_count;
/* what each service was made with: its usage page and usage, its callbacks and their refcon */
static struct {
	long long page, usage;
	const struct virtual_service_callbacks *callbacks;
	void *refcon;
} made_services[8];

IOHIDEventSystemClientRef
IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes)
{
	(void)allocator; (void)type; (void)attributes;
	return &the_client;
}

void
IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue)
{
	(void)client; (void)queue;
}

IOHIDServiceClientRef
IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon)
{
	(void)client; (void)attributes; (void)target;
	((struct service *)refcon)->opened = true;     /* (the event server takes it at once) */
	made_services[service_count % 8].page = given_page;
	made_services[service_count % 8].usage = given_usage;
	made_services[service_count % 8].callbacks = callbacks;
	made_services[service_count % 8].refcon = refcon;
	return &services[service_count++ % 8];
}

Boolean
IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service)
{
	(void)service;
	return true;
}

/* what reached the event server, in order */
static struct sent {
	struct __IOHIDEvent event, child[16];
} sent[256];
static unsigned sent_count;

Boolean
IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event)
{
	(void)service;
	if (sent_count == sizeof(sent) / sizeof(sent[0])) {
		abort();
	}
	sent[sent_count].event = *event;
	for (unsigned c = 0; c < event->children; c++) {
		sent[sent_count].child[c] = *event->child[c];
	}
	sent_count++;
	return true;
}

IOHIDEventRef
IOHIDEventCreateDigitizerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t transducer, uint32_t index, uint32_t identity,
    uint32_t event_mask, uint32_t button_mask, double x, double y, double z, double tip_pressure, double barrel_pressure,
    double twist, Boolean range, Boolean touch, uint32_t options)
{
	struct __IOHIDEvent *e = made(event_new());

	(void)allocator; (void)time; (void)transducer; (void)identity; (void)button_mask; (void)z; (void)tip_pressure;
	(void)barrel_pressure; (void)twist; (void)options;
	e->mask = event_mask;
	e->index = index;
	e->x = x;
	e->y = y;
	e->range = range;
	e->touch = touch;
	return e;
}

IOHIDEventRef
IOHIDEventCreateDigitizerFingerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t index, uint32_t identity,
    uint32_t event_mask, double x, double y, double z, double tip_pressure, double twist, Boolean range, Boolean touch,
    uint32_t options)
{
	struct __IOHIDEvent *e = made(event_new());

	(void)allocator; (void)time; (void)identity; (void)z; (void)tip_pressure; (void)twist; (void)options;
	e->mask = event_mask;
	e->index = index;
	e->x = x;
	e->y = y;
	e->range = range;
	e->touch = touch;
	return e;
}

IOHIDEventRef
IOHIDEventCreateKeyboardEvent(CFAllocatorRef allocator, uint64_t time, uint32_t usage_page, uint32_t usage, Boolean down,
    uint32_t options)
{
	struct __IOHIDEvent *e = made(event_new());

	(void)allocator; (void)time; (void)usage_page; (void)options;
	e->keyboard = true;
	e->usage = usage;
	e->down = down;
	return e;
}

void
IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options)
{
	(void)options;
	if (parent->children == 16) {
		abort();
	}
	child->references++;            /* (the parent holds its children) */
	parent->child[parent->children++] = child;
}

void
IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value)
{
	if (event->field_count < sizeof(event->fields) / sizeof(event->fields[0])) {
		event->fields[event->field_count].field = field;
		event->fields[event->field_count++].value = value;
	}
}

/* A client (locationd) sets the "ReportInterval" of the service made last, through the service's own callbacks. */
static void
client_asks(long long interval)
{
	unsigned last = (service_count - 1) % 8;

	if (service_count == 0) {
		abort();
	}
	made_services[last].callbacks->setProperty(NULL, made_services[last].refcon, &services[last], CFSTR("ReportInterval"),
	    CFNumberCreate(NULL, kCFNumberSInt64Type, &interval));
}

/* IOHIDEventCreate(), IOHIDEventCreateVendorDefinedEvent(), IOHIDEventCreateAccelerometerEvent() and IOHIDEventCreateGyroEvent(), as
 * the part finds them by name */
static IOHIDEventRef
test_event_create(CFAllocatorRef allocator, uint32_t type, uint64_t time, uint32_t options)
{
	struct __IOHIDEvent *e = made(event_new());

	(void)allocator; (void)options;
	e->type = type;
	e->time = time;
	return e;
}

static IOHIDEventRef
test_vendor_create(CFAllocatorRef allocator, uint64_t time, uint32_t page, uint32_t usage, uint32_t version, const uint8_t *data,
    CFIndex length, uint32_t options)
{
	struct __IOHIDEvent *e = made(event_new());

	(void)allocator; (void)version; (void)options;
	e->page = page;
	e->usage = usage;
	e->time = time;
	e->length = length;
	memcpy(e->data, data, length < 8 ? (size_t)length : 8);
	return e;
}

static IOHIDEventRef
test_accelerometer_create(CFAllocatorRef allocator, uint64_t time, double x, double y, double z, uint32_t options)
{
	struct __IOHIDEvent *e = made(event_new());

	(void)allocator; (void)options;
	e->accelerometer = true;
	e->time = time;
	e->x = x;
	e->y = y;
	e->z = z;
	return e;
}

static IOHIDEventRef
test_gyro_create(CFAllocatorRef allocator, uint64_t time, double x, double y, double z, uint32_t options)
{
	struct __IOHIDEvent *e = made(event_new());

	(void)allocator; (void)options;
	e->gyro = true;
	e->time = time;
	e->x = x;
	e->y = y;
	e->z = z;
	return e;
}

static void *
test_dlsym(void *handle, const char *name)
{
	(void)handle;
	if (hid_calls_missing) {
		return NULL;
	}
	if (strcmp(name, "IOHIDEventCreateGyroEvent") == 0) {
		return gyro_call_missing ? NULL : (void *)test_gyro_create;
	}
	if (strcmp(name, "IOHIDEventCreateAccelerometerEvent") == 0) {
		return accelerometer_call_missing ? NULL : (void *)test_accelerometer_create;
	}
	if (strcmp(name, "IOHIDEventCreate") == 0) {
		return (void *)test_event_create;
	}
	if (strcmp(name, "IOHIDEventCreateVendorDefinedEvent") == 0) {
		return (void *)test_vendor_create;
	}
	return NULL;
}

/* the value a field of an event was last set to, or -1 */
static CFIndex
field_of(const struct __IOHIDEvent *e, uint32_t field)
{
	CFIndex value = -1;

	for (unsigned i = 0; i < e->field_count; i++) {
		if (e->fields[i].field == field) {
			value = e->fields[i].value;
		}
	}
	return value;
}

/* ---- the scenarios ---------------------------------------------------------------------------- */

static int failures;

#define CHECK(condition, ...) do { \
	if (!(condition)) { \
		printf("FAIL: %s:%d: ", __func__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		failures++; \
	} \
} while (0)

static void
opening(int e)
{
	opens[open_scripted++] = e;
}

static void
sample(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	reads[read_count].error = 0;
	reads[read_count].t = (struct hakios_touch){ .time = 500 + read_count, .type = type, .finger = finger, .x = x, .y = y };
	read_count++;
}

static void
read_error(int e)
{
	reads[read_count++].error = e;
}

/* the touch screen's frames, the keys' events left out: the n-th of them, or NULL */
static const struct sent *
frame(unsigned n)
{
	for (unsigned i = 0; i < sent_count; i++) {
		if (!sent[i].event.keyboard && sent[i].event.type != HID_EVENT_ORIENTATION && !sent[i].event.accelerometer &&
		    !sent[i].event.gyro && n-- == 0) {
			return &sent[i];
		}
	}
	return NULL;
}

static unsigned
frames(void)
{
	unsigned n = 0;

	while (frame(n) != NULL) {
		n++;
	}
	return n;
}

/* the finger (from 0) in a frame, or NULL */
static const struct __IOHIDEvent *
finger_in(const struct sent *s, uint32_t finger)
{
	for (unsigned c = 0; s != NULL && c < s->event.children; c++) {
		if (s->child[c].index == finger + 1) {
			return &s->child[c];
		}
	}
	return NULL;
}

/* Every frame: the hand touches while one of its fingers does, and no finger but one that touches or has just lifted is in it. */
static void
check_frames_agree(void)
{
	for (unsigned n = 0; frame(n) != NULL; n++) {
		const struct sent *s = frame(n);
		bool any = false;
		unsigned changed = 0;

		for (unsigned c = 0; c < s->event.children; c++) {
			any |= s->child[c].touch;
			changed += (s->child[c].mask & (DIGITIZER_RANGE | DIGITIZER_TOUCH | DIGITIZER_POSITION)) != 0;
			CHECK(s->child[c].range == s->child[c].touch, "frame %u: finger %u in range %d, touching %d", n,
			    s->child[c].index - 1, s->child[c].range, s->child[c].touch);
			CHECK(s->child[c].x >= 0 && s->child[c].x <= 1 && s->child[c].y >= 0 && s->child[c].y <= 1,
			    "frame %u: finger %u at %f %f, off the screen", n, s->child[c].index - 1, s->child[c].x, s->child[c].y);
		}
		CHECK(s->event.touch == any && s->event.range == any, "frame %u: the hand touches %d, in range %d, its fingers %d",
		    n, s->event.touch, s->event.range, any);
		CHECK(changed == 1, "frame %u: %u fingers changed", n, changed);
	}
}

static void
run(void)
{
	input_thread(NULL);
	check_frames_agree();
	CHECK(live_events == 0, "%d events never let go of", live_events);
}

/* Two fingers down, moved and lifted; samples for no finger that is down, or for a finger past the ten, change nothing. */
static void
scenario_fingers(void)
{
	opening(0);
	sample(HAKIOS_TOUCH_UP, 3, 100, 100);           /* lifted without having landed */
	sample(HAKIOS_TOUCH_MOVE, 4, 100, 100);         /* moved without having landed */
	sample(HAKIOS_TOUCH_DOWN, 12, 100, 100);        /* past the fingers there are */
	sample(HAKIOS_TOUCH_DOWN, 0, 32768, 32768);
	sample(HAKIOS_TOUCH_DOWN, 1, 65535, 65535);
	sample(HAKIOS_TOUCH_MOVE, 1, 65535, 0);
	sample(HAKIOS_TOUCH_UP, 0, 32768, 32768);
	sample(HAKIOS_TOUCH_UP, 1, 65535, 0);
	run();
	CHECK(frames() == 5, "%u frames, not 5", frames());
	CHECK(finger_in(frame(0), 0) && finger_in(frame(0), 0)->touch && !finger_in(frame(0), 1), "the first finger's landing");
	CHECK(finger_in(frame(1), 1) && finger_in(frame(1), 1)->x == 1.0 && finger_in(frame(1), 1)->y == 1.0,
	    "65535 is the screen's far edge, 1.0");
	CHECK(finger_in(frame(1), 0) && finger_in(frame(1), 0)->touch, "the first finger still down beside the second");
	CHECK(finger_in(frame(2), 1) && finger_in(frame(2), 1)->mask & DIGITIZER_POSITION, "the second finger moved");
	CHECK(finger_in(frame(3), 0) && !finger_in(frame(3), 0)->touch && frame(3)->event.touch, "the first lifted, the hand still down");
	CHECK(frame(4) && !frame(4)->event.touch && finger_in(frame(4), 1) && !finger_in(frame(4), 1)->touch, "both lifted");
}

/* A signal interrupts a read: the touches after it still reach the screen. */
static void
scenario_interrupted(void)
{
	opening(0);
	sample(HAKIOS_TOUCH_DOWN, 0, 1000, 30000);
	read_error(EINTR);
	sample(HAKIOS_TOUCH_UP, 0, 1000, 30000);
	run();
	CHECK(frames() == 2, "%u frames, not 2: the read interrupted ended the touches", frames());
	CHECK(frame(1) != NULL && !frame(1)->event.touch, "the finger is never lifted");
}

/* A read fails: what is held is let go, the device is opened again, and its touches go on. */
static void
scenario_read_fails(void)
{
	opening(0);
	opening(0);
	sample(HAKIOS_TOUCH_DOWN, 0, 1000, 30000);
	sample(HAKIOS_BUTTON_DOWN, HAKIOS_BUTTON_LOCK, 0, 0);
	read_error(EIO);
	sample(HAKIOS_TOUCH_DOWN, 1, 2000, 2000);
	sample(HAKIOS_TOUCH_UP, 1, 2000, 2000);
	run();
	CHECK(opened == 2, "the device opened %u times, not twice", opened);
	CHECK(frames() == 4, "%u frames, not 4", frames());
	CHECK(frame(1) != NULL && !frame(1)->event.touch && finger_in(frame(1), 0) && !finger_in(frame(1), 0)->touch &&
	    fabs(finger_in(frame(1), 0)->x - 1000 / 65535.0) < 1e-6, "the finger held when the read failed is not lifted where it was");
	CHECK(finger_in(frame(2), 1) && finger_in(frame(2), 1)->touch, "the touches after the read failed do not reach the screen");
	{
		unsigned presses = 0, releases = 0;

		for (unsigned i = 0; i < sent_count; i++) {
			if (sent[i].event.keyboard && sent[i].event.usage == HID_USAGE_POWER) {
				presses += sent[i].event.down;
				releases += !sent[i].event.down;
			}
		}
		CHECK(presses == 1 && releases == 1, "the side button pressed %u times, released %u", presses, releases);
		CHECK(lock_state == 1, "the side button is said to be held");
	}
}

/* Another program reads the device for a while: the touches are read once it has let go. */
static void
scenario_busy(void)
{
	opening(EBUSY);
	opening(EBUSY);
	opening(0);
	sample(HAKIOS_TOUCH_DOWN, 2, 40000, 40000);
	sample(HAKIOS_TOUCH_UP, 2, 40000, 40000);
	run();
	CHECK(opened == 1, "the device never opened");
	CHECK(frames() == 2, "%u frames, not 2", frames());
}

/* ---- the phone's orientation (hid_orientation.h) ----------------------------------------------- */

/* what the kernel says of a number, in turn, the last for good (-99: a read that fails) */
static void
kernel_says(struct number_script *script, unsigned count, const int *values)
{
	memcpy(script->values, values, count * sizeof(int));
	script->count = count;
	script->next = 0;
}

/* the orientation events that reached the event server, the n-th of them, or NULL */
static const struct sent *
orientation_event(unsigned n)
{
	for (unsigned i = 0; i < sent_count; i++) {
		if (sent[i].event.type == HID_EVENT_ORIENTATION && n-- == 0) {
			return &sent[i];
		}
	}
	return NULL;
}

/* An orientation event as locationd reads it: a usage, the orientation's, built in, and its time in its one child. */
static void
check_orientation_event(unsigned n, uint32_t usage)
{
	const struct sent *s = orientation_event(n);
	uint64_t when = 0;

	CHECK(s != NULL, "no orientation event %u", n);
	if (s == NULL) {
		return;
	}
	CHECK(field_of(&s->event, FIELD_ORIENTATION_KIND) == ORIENTATION_KIND_USAGE, "event %u: given as a usage (1): %ld", n,
	    (long)field_of(&s->event, FIELD_ORIENTATION_KIND));
	CHECK(field_of(&s->event, FIELD_ORIENTATION_USAGE) == (CFIndex)usage, "event %u: usage %#x, not %#lx", n, usage,
	    (long)field_of(&s->event, FIELD_ORIENTATION_USAGE));
	CHECK(field_of(&s->event, FIELD_IS_BUILT_IN) == 1, "event %u: not built in", n);
	CHECK(s->event.children == 1, "event %u: %u children, not the time alone", n, s->event.children);
	if (s->event.children != 1) {
		return;
	}
	CHECK(s->child[0].page == HID_PAGE_SENSOR_TIME && s->child[0].usage == HID_USAGE_SENSOR_TIME && s->child[0].length == 8,
	    "event %u: its child is page %#x usage %u, %ld bytes", n, s->child[0].page, s->child[0].usage, (long)s->child[0].length);
	for (unsigned i = 0; i < 8; i++) {
		when |= (uint64_t)s->child[0].data[i] << (8 * i);
	}
	CHECK(when == s->event.time && when == s->child[0].time, "event %u: its time %llu, the child's bytes say %llu", n,
	    (unsigned long long)s->event.time, (unsigned long long)when);
	if (n > 0 && orientation_event(n - 1) != NULL) {
		CHECK(s->event.time > orientation_event(n - 1)->event.time, "event %u: not later than the one before", n);
	}
}

/*
 * The kernel follows the orientation for Darwin: the service is made, and each known orientation it changes to is
 * said once, as its usage -- not "not known", not a read that failed, not the same one again at once; once the kernel
 * says it no longer follows it, the looking stops.
 */
static void
scenario_orientation_followed(void)
{
	static const int hid[] = { 1, 1, 1, 1, 1, 1, 1, 1, 0 };
	static const int orientation[] = { 0, 1, 1, 3, -99, 3, 5 };

	kernel_says(&kernel_hid, 9, hid);
	kernel_says(&kernel_orientation, 7, orientation);
	orientation_thread(&the_client);
	CHECK(service_count == 1, "%u services made, not the orientation's alone", service_count);
	CHECK(kernel_orientation.asked == 7, "the orientation looked at %u times, not 7 (while the kernel said 1)",
	    kernel_orientation.asked);
	CHECK(orientation_event(3) == NULL, "more than three orientations said");
	check_orientation_event(0, 0x65);       /* portrait */
	check_orientation_event(1, 0x67);       /* landscape left */
	check_orientation_event(2, 0x69);       /* face up */
	CHECK(frames() == 0, "%u touch frames", frames());
	CHECK(live_events == 0, "%d events never let go of", live_events);
}

/* What is said when, apart from the service: again after ORIENTATION_AGAIN_MS, at once on a change, never "not known". */
static void
scenario_orientation_said_again(void)
{
	struct orientation_feed f = { 0 };

	CHECK(orientation_feed_next(&f, 0, 0) == 0, "\"not known\" said");
	CHECK(orientation_feed_next(&f, 1, 0) == 1, "the first known one not said");
	CHECK(orientation_feed_next(&f, 1, ORIENTATION_AGAIN_MS - 1) == 0, "said again too soon");
	CHECK(orientation_feed_next(&f, 1, ORIENTATION_AGAIN_MS) == 1, "not said again after %d ms", ORIENTATION_AGAIN_MS);
	CHECK(orientation_feed_next(&f, 0, ORIENTATION_AGAIN_MS * 3) == 0 && f.sent == 1, "\"not known\" took the place of portrait");
	CHECK(orientation_feed_next(&f, -1, ORIENTATION_AGAIN_MS * 3) == 0 && orientation_feed_next(&f, 7, 0) == 0,
	    "a read that failed, or a number past face down, said");
	CHECK(orientation_feed_next(&f, 4, ORIENTATION_AGAIN_MS + 1) == 4, "a change not said at once");
	CHECK(orientation_feed_stamp(&f, 100) == 100 && orientation_feed_stamp(&f, 100) == 101 && orientation_feed_stamp(&f, 50) == 102 &&
	    orientation_feed_stamp(&f, 500) == 500, "the times carried do not only grow");
	CHECK(orientation_usage(0) == 0x64 && orientation_usage(1) == 0x65 && orientation_usage(6) == 0x6a, "the usages");
}

/*
 * The kernel does not say it follows the orientation (sensors=2 off, or orientation_hid=0): looked at once a second for
 * ORIENTATION_WAIT_S, then no more, and no service; one that comes to say so within that is waited for.
 */
static void
scenario_orientation_waits_for_the_kernel(void)
{
	static const int never[] = { 0 }, later[] = { 0, 0, 1, 1, 0 }, face_down[] = { 6 };

	kernel_says(&kernel_hid, 1, never);
	orientation_thread(&the_client);
	CHECK(kernel_hid.asked == ORIENTATION_WAIT_S + 1, "asked %u times, not %d", kernel_hid.asked, ORIENTATION_WAIT_S + 1);
	CHECK(service_count == 0 && kernel_orientation.asked == 0, "a service made, or the orientation looked at, without the kernel");
	kernel_says(&kernel_hid, 5, later);
	kernel_says(&kernel_orientation, 1, face_down);
	orientation_thread(&the_client);
	CHECK(service_count == 1, "%u services made once the kernel said it follows the orientation", service_count);
	check_orientation_event(0, 0x6a);
	CHECK(orientation_event(1) == NULL && live_events == 0, "more than face down, or events not let go of");
}

/* A Darwin whose IOKit has not got one of the two calls: no service, nothing sent, the kernel's orientation not read. */
static void
scenario_orientation_without_the_calls(void)
{
	static const int hid[] = { 1, 1, 0 }, portrait[] = { 1 };

	hid_calls_missing = true;
	kernel_says(&kernel_hid, 3, hid);
	kernel_says(&kernel_orientation, 1, portrait);
	orientation_thread(&the_client);
	CHECK(service_count == 0 && sent_count == 0 && kernel_orientation.asked == 0, "%u services, %u events, the orientation read %u "
	    "times", service_count, sent_count, kernel_orientation.asked);
}

/* A kernel that says nothing of the orientation (no hw.sensors.hid): the touch screen and the buttons alone, as before. */
static void
scenario_orientation_not_without_the_kernel(void)
{
	opening(0);
	sample(HAKIOS_TOUCH_DOWN, 0, 1000, 1000);
	sample(HAKIOS_TOUCH_UP, 0, 1000, 1000);
	run();
	CHECK(service_count == 2, "%u services, not the touch screen and the buttons", service_count);
	CHECK(orientation_event(0) == NULL && frames() == 2, "an orientation said, or the touches lost");
}

/* ---- the accelerometer's samples (hid_motion.h) ------------------------------------------------- */

/* what hw.sensors.sample says, in turn, the last for good (NULL: a read that fails); before the look that would read
 * the n-th, a client may set the service's "ReportInterval" (sample_asked) */
static void
samples_say(unsigned count, const char *const *texts)
{
	for (unsigned i = 0; i < count; i++) {
		kernel_sample.texts[i] = texts[i];
	}
	kernel_sample.count = count;
	kernel_sample.next = 0;
}

static void
sample_asked(unsigned n, long long interval)
{
	kernel_sample.asked_before[n] = interval;
	kernel_sample.asks_before[n] = true;
}

/* the accelerometer events that reached the event server, the n-th of them, or NULL */
static const struct sent *
accelerometer_event(unsigned n)
{
	for (unsigned i = 0; i < sent_count; i++) {
		if (sent[i].event.accelerometer && n-- == 0) {
			return &sent[i];
		}
	}
	return NULL;
}

static unsigned
accelerometer_events(void)
{
	unsigned n = 0;

	while (accelerometer_event(n) != NULL) {
		n++;
	}
	return n;
}

/* An accelerometer event as locationd reads it: x y z in g, built in, dated when the kernel read the sample. */
static void
check_accelerometer_event(unsigned n, uint64_t time, double x, double y, double z)
{
	const struct sent *s = accelerometer_event(n);

	CHECK(s != NULL, "no accelerometer event %u", n);
	if (s == NULL) {
		return;
	}
	CHECK(s->event.time == time, "event %u: dated %llu, not %llu (when the kernel read it)", n, (unsigned long long)s->event.time,
	    (unsigned long long)time);
	CHECK(fabs(s->event.x - x) < 1e-9 && fabs(s->event.y - y) < 1e-9 && fabs(s->event.z - z) < 1e-9,
	    "event %u: %f %f %f g, not %f %f %f", n, s->event.x, s->event.y, s->event.z, x, y, z);
	CHECK(field_of(&s->event, FIELD_IS_BUILT_IN) == 1, "event %u: not built in", n);
	CHECK(s->event.children == 0 && s->event.type == 0, "event %u: %u children, type %u", n, s->event.children, s->event.type);
}

/*
 * The kernel gives the samples (hw.sensors.hid and hw.sensors.motion 1): the service is made on Apple's page 0xff00,
 * usage 3, and each sample the kernel has read is sent once, in g, dated when it was read -- not one not read yet
 * ("0 ..."), not one read twice, not a read that failed, not a text that is not five numbers; once the kernel says
 * the samples are not for Darwin any more, the looking stops.
 */
static void
scenario_motion_followed(void)
{
	static const int hid[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 }, motion[] = { 1 };
	static const char *const texts[] = {
		"0 0 0 0 0", "1 5000000 0 -1000 0", "1 5000000 0 -1000 0", NULL, "2 105000000 500 500 -707", "3 205000000",
		"4 305000000 -12 3 -1001",
	};

	kernel_says(&kernel_hid, 17, hid);
	kernel_says(&kernel_motion, 1, motion);
	samples_say(7, texts);
	motion_thread(&the_client);
	CHECK(service_count == 1, "%u services made, not the accelerometer's alone", service_count);
	CHECK(made_services[0].page == 0xff00 && made_services[0].usage == 0x03, "the service is page %#llx usage %#llx, not Apple's "
	    "accelerometer (0xff00, 3)", made_services[0].page, made_services[0].usage);
	CHECK(accelerometer_events() == 3, "%u samples sent, not 3", accelerometer_events());
	check_accelerometer_event(0, 5000000, 0, -1, 0);
	check_accelerometer_event(1, 105000000, 0.5, 0.5, -0.707);
	check_accelerometer_event(2, 305000000, -0.012, 0.003, -1.001);
	CHECK(kernel_sample.asked >= 7, "the kernel's sample looked at %u times", kernel_sample.asked);
	CHECK(frames() == 0 && orientation_event(0) == NULL, "touch frames or orientations sent");
	CHECK(live_events == 0, "%d events never let go of", live_events);
}

/*
 * What a client asks of the service ("ReportInterval") is what is sent: every sample while nobody has asked or what
 * was asked says "not configured", every other at 200 ms, every one at 100 ms or less -- the kernel's own rate -- and
 * none while it is 0, when the kernel's sample is not even looked at.
 */
static void
scenario_motion_as_asked(void)
{
	static const int hid[] = { 1 }, motion[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 };
	static const char *const texts[] = {
		"1 100000000 0 0 -1000", "2 200000000 0 0 -1000", "3 300000000 0 0 -1000", "4 400000000 0 0 -1000",
		"5 500000000 0 0 -1000", "6 600000000 0 0 -1000", "7 700000000 0 0 -1000", "8 800000000 0 0 -1000",
		"9 900000000 0 0 -1000", "10 1000000000 0 0 -1000",
	};

	kernel_says(&kernel_hid, 1, hid);
	kernel_says(&kernel_motion, 23, motion);
	samples_say(10, texts);
	sample_asked(1, 0xffffffffLL);          /* "not configured": every one (1 before it, as nobody had asked, and 2) */
	sample_asked(2, 200000);                /* 5 a second: every other (not 3, 4, not 5) */
	sample_asked(5, 10000);                 /* 100 a second: every one the kernel has (6) */
	sample_asked(6, 0);                     /* none: 7 is never read */
	motion_thread(&the_client);
	CHECK(accelerometer_events() == 4, "%u samples sent, not 4", accelerometer_events());
	for (unsigned n = 0; n < 4; n++) {
		static const uint64_t times[] = { 100000000, 200000000, 400000000, 600000000 };

		check_accelerometer_event(n, times[n], 0, 0, -1);
	}
	CHECK(kernel_sample.asked == 6, "the kernel's sample looked at %u times, not 6: looked at while none was wanted",
	    kernel_sample.asked);
	CHECK(live_events == 0, "%d events never let go of", live_events);
}

/* What is sent when, apart from the service. */
static void
scenario_motion_feed(void)
{
	struct motion_feed f = { 0 };

	CHECK(motion_interval(-1) == -1 && motion_interval(0xffffffffLL) == -1 && motion_interval(0x100000000LL) == -1,
	    "\"not configured\" taken for a rate");
	CHECK(motion_interval(0) == 0 && motion_interval(10000) == 10000, "a rate asked not taken");
	CHECK(!motion_feed_take(&f, 0, 0, -1), "a sample not read yet sent");
	CHECK(motion_feed_take(&f, 1, 100000, -1) && !motion_feed_take(&f, 1, 100000, -1), "the first not sent, or sent twice");
	CHECK(!motion_feed_take(&f, 2, 200000, 0) && motion_feed_take(&f, 3, 300000, 200000), "sent while none was wanted, or the "
	    "first after not");
	CHECK(!motion_feed_take(&f, 4, 401000, 200000) && motion_feed_take(&f, 5, 502000, 200000), "every other not sent at 200 ms");
	CHECK(motion_feed_take(&f, 6, 603000, 100000) && motion_feed_take(&f, 7, 704000, 100000), "not every one at the kernel's rate");
	/* 150 ms: the kernel's every one (never slower than asked), 1 s: every tenth */
	CHECK(motion_feed_take(&f, 8, 805000, 150000) && motion_feed_take(&f, 9, 906000, 150000), "not every one at 150 ms");
	CHECK(!motion_feed_take(&f, 10, 1007000, 1000000) && !motion_feed_take(&f, 18, 1806000, 1000000) &&
	    motion_feed_take(&f, 19, 1906000, 1000000), "not every tenth at a second");
}

/*
 * The kernel follows the orientation for Darwin but keeps the samples from its motion stack (motion_hid=0), or says
 * nothing of them (a kernel from before hw.sensors.motion): no accelerometer service, no sample read.
 */
static void
scenario_motion_kept_from_darwin(void)
{
	static const int hid[] = { 1 }, off[] = { 0 };
	static const char *const texts[] = { "1 5000000 0 -1000 0" };

	kernel_says(&kernel_hid, 1, hid);
	kernel_says(&kernel_motion, 1, off);
	samples_say(1, texts);
	motion_thread(&the_client);
	CHECK(service_count == 0 && kernel_sample.asked == 0 && sent_count == 0, "%u services, the sample read %u times, %u events",
	    service_count, kernel_sample.asked, sent_count);
	kernel_motion.count = 0;
	motion_thread(&the_client);
	CHECK(service_count == 0 && kernel_sample.asked == 0, "without hw.sensors.motion: %u services, the sample read %u times",
	    service_count, kernel_sample.asked);
}

/* The kernel does not follow the sensor for Darwin (hw.sensors.hid never 1): waited for as the orientation's, then nothing. */
static void
scenario_motion_waits_for_the_kernel(void)
{
	static const int never[] = { 0 }, motion[] = { 1 };

	kernel_says(&kernel_hid, 1, never);
	kernel_says(&kernel_motion, 1, motion);
	motion_thread(&the_client);
	CHECK(kernel_hid.asked == ORIENTATION_WAIT_S + 1, "asked %u times, not %d", kernel_hid.asked, ORIENTATION_WAIT_S + 1);
	CHECK(service_count == 0 && kernel_sample.asked == 0, "a service made, or a sample read, without the kernel");
}

/* A Darwin whose IOKit has not got IOHIDEventCreateAccelerometerEvent: no service, nothing sent, nothing read. */
static void
scenario_motion_without_the_call(void)
{
	static const int one[] = { 1, 1, 0 };
	static const char *const texts[] = { "1 5000000 0 -1000 0" };

	accelerometer_call_missing = true;
	kernel_says(&kernel_hid, 3, one);
	kernel_says(&kernel_motion, 3, one);
	samples_say(1, texts);
	motion_thread(&the_client);
	CHECK(service_count == 0 && sent_count == 0 && kernel_sample.asked == 0, "%u services, %u events, the sample read %u times",
	    service_count, sent_count, kernel_sample.asked);
}

static const struct sent *gyro_event(unsigned n);

/* The kernel's samples with the gyroscope's rates after the five: the accelerometer's service reads its five as ever. */
static void
scenario_motion_reads_past_the_rates(void)
{
	static const int hid[] = { 1, 1, 1, 1, 0 }, motion[] = { 1 };
	static const char *const texts[] = { "1 5000000 0 -1000 0 1222 -40 3", "2 105000000 500 500 -707 0 0 0" };

	kernel_says(&kernel_hid, 5, hid);
	kernel_says(&kernel_motion, 1, motion);
	samples_say(2, texts);
	motion_thread(&the_client);
	CHECK(accelerometer_events() == 2, "%u samples sent, not 2", accelerometer_events());
	check_accelerometer_event(0, 5000000, 0, -1, 0);
	check_accelerometer_event(1, 105000000, 0.5, 0.5, -0.707);
	CHECK(gyro_event(0) == NULL, "the accelerometer's service sent rates");
	CHECK(live_events == 0, "%d events never let go of", live_events);
}

/* ---- the gyroscope's rates (hid_gyro.h) ------------------------------------------------------- */

/* the gyroscope events that reached the event server, the n-th of them, or NULL */
static const struct sent *
gyro_event(unsigned n)
{
	for (unsigned i = 0; i < sent_count; i++) {
		if (sent[i].event.gyro && n-- == 0) {
			return &sent[i];
		}
	}
	return NULL;
}

static unsigned
gyro_events(void)
{
	unsigned n = 0;

	while (gyro_event(n) != NULL) {
		n++;
	}
	return n;
}

/* A gyroscope event as locationd reads it: x y z in rad/s, built in, dated when the kernel read the sample. */
static void
check_gyro_event(unsigned n, uint64_t time, double x, double y, double z)
{
	const struct sent *s = gyro_event(n);

	CHECK(s != NULL, "no gyroscope event %u", n);
	if (s == NULL) {
		return;
	}
	CHECK(s->event.time == time, "event %u: dated %llu, not %llu (when the kernel read it)", n, (unsigned long long)s->event.time,
	    (unsigned long long)time);
	CHECK(fabs(s->event.x - x) < 1e-9 && fabs(s->event.y - y) < 1e-9 && fabs(s->event.z - z) < 1e-9,
	    "event %u: %f %f %f rad/s, not %f %f %f", n, s->event.x, s->event.y, s->event.z, x, y, z);
	CHECK(field_of(&s->event, FIELD_IS_BUILT_IN) == 1, "event %u: not built in", n);
	CHECK(!s->event.accelerometer && s->event.children == 0 && s->event.type == 0, "event %u: an accelerometer's %d, %u children, "
	    "type %u", n, s->event.accelerometer, s->event.children, s->event.type);
}

/*
 * The kernel gives the gyroscope's rates (hw.sensors.hid, .motion and .gyro 1): the service is made on Apple's page
 * 0xff00, usage 9, and each sample that carries rates is sent once, in rad/s (the kernel's milli-rad/s over 1000), on
 * the axes the kernel gives, dated when it was read -- not one not read yet, not one read twice, not a read that
 * failed, not a sample without rates (five numbers) nor one of seven; once the kernel says the rates are not for
 * Darwin any more (here hw.sensors.gyro 0: the gyroscope held stuck), the looking stops though the samples go on.
 */
static void
scenario_gyro_followed(void)
{
	static const int one[] = { 1 }, gyro[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 };
	static const char *const texts[] = {
		"0 0 0 0 0", "1 5000000 0 -1000 0", "2 105000000 0 -1000 0 1222 -40 3", "2 105000000 0 -1000 0 1222 -40 3", NULL,
		"3 205000000 0 0 -1000", "4 305000000 0 0 -1000 -2443 0 40034", "5 405000000 0 0 -1000 1 2",
		"6 505000000 0 0 -1000 0 0 -1",
	};

	kernel_says(&kernel_hid, 1, one);
	kernel_says(&kernel_motion, 1, one);
	kernel_says(&kernel_gyro, 11, gyro);
	samples_say(9, texts);
	gyro_thread(&the_client);
	CHECK(service_count == 1, "%u services made, not the gyroscope's alone", service_count);
	CHECK(made_services[0].page == 0xff00 && made_services[0].usage == 0x09, "the service is page %#llx usage %#llx, not Apple's "
	    "gyroscope (0xff00, 9)", made_services[0].page, made_services[0].usage);
	CHECK(gyro_events() == 3, "%u rates sent, not 3", gyro_events());
	check_gyro_event(0, 105000000, 1.222, -0.04, 0.003);
	check_gyro_event(1, 305000000, -2.443, 0, 40.034);
	check_gyro_event(2, 505000000, 0, 0, -0.001);
	CHECK(kernel_sample.asked == 9, "the kernel's sample looked at %u times, not 9: looked at after hw.sensors.gyro said 0",
	    kernel_sample.asked);
	CHECK(accelerometer_events() == 0 && frames() == 0 && orientation_event(0) == NULL, "accelerometer events, touch frames or "
	    "orientations sent");
	CHECK(live_events == 0, "%d events never let go of", live_events);
}

/*
 * What a client asks of the gyroscope's service ("ReportInterval") is what is sent, as for the accelerometer's: every
 * sample while what was asked says "not configured", every other at 200 ms, every one at 10 ms (the kernel reads no more
 * often), none while it is 0, when the kernel's sample is not even looked at.
 */
static void
scenario_gyro_as_asked(void)
{
	static const int one[] = { 1 }, motion[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 };
	static const char *const texts[] = {
		"1 100000000 0 0 -1000 10 0 0", "2 200000000 0 0 -1000 20 0 0", "3 300000000 0 0 -1000 30 0 0",
		"4 400000000 0 0 -1000 40 0 0", "5 500000000 0 0 -1000 50 0 0", "6 600000000 0 0 -1000 60 0 0",
		"7 700000000 0 0 -1000 70 0 0", "8 800000000 0 0 -1000 80 0 0",
	};

	kernel_says(&kernel_hid, 1, one);
	kernel_says(&kernel_motion, 23, motion);
	kernel_says(&kernel_gyro, 1, one);
	samples_say(8, texts);
	sample_asked(1, 0xffffffffLL);          /* "not configured": every one (1 before it, as nobody had asked, and 2) */
	sample_asked(2, 200000);                /* 5 a second: every other (not 3, 4, not 5) */
	sample_asked(5, 10000);                 /* 100 a second: every one the kernel has (6) */
	sample_asked(6, 0);                     /* none: 7 is never read */
	gyro_thread(&the_client);
	CHECK(gyro_events() == 4, "%u rates sent, not 4", gyro_events());
	for (unsigned n = 0; n < 4; n++) {
		static const uint64_t times[] = { 100000000, 200000000, 400000000, 600000000 };
		static const double x[] = { 0.01, 0.02, 0.04, 0.06 };

		check_gyro_event(n, times[n], x[n], 0, 0);
	}
	CHECK(kernel_sample.asked == 6, "the kernel's sample looked at %u times, not 6: looked at while none was wanted",
	    kernel_sample.asked);
	CHECK(live_events == 0, "%d events never let go of", live_events);
}

/*
 * The kernel gives no rates: gyro_hid=0, or a gyroscope it has not seen answer (hw.sensors.gyro 0 throughout, waited
 * for as hw.sensors.hid is), a kernel from before hw.sensors.gyro, the samples kept from the motion stack
 * (motion_hid=0), or taken from it while the rates are waited for: no gyroscope service, no sample read.
 */
static void
scenario_gyro_kept_from_darwin(void)
{
	static const int one[] = { 1 }, off[] = { 0 }, motion_goes[] = { 1, 1, 1, 0 };
	static const char *const texts[] = { "1 5000000 0 -1000 0 1222 -40 3" };

	kernel_says(&kernel_hid, 1, one);
	kernel_says(&kernel_motion, 1, one);
	kernel_says(&kernel_gyro, 1, off);
	samples_say(1, texts);
	gyro_thread(&the_client);
	CHECK(kernel_gyro.asked == ORIENTATION_WAIT_S + 1, "hw.sensors.gyro asked %u times, not %d", kernel_gyro.asked,
	    ORIENTATION_WAIT_S + 1);
	CHECK(service_count == 0 && kernel_sample.asked == 0 && sent_count == 0, "hw.sensors.gyro 0: %u services, the sample read "
	    "%u times, %u events", service_count, kernel_sample.asked, sent_count);
	kernel_gyro.count = 0;
	gyro_thread(&the_client);
	CHECK(service_count == 0 && kernel_sample.asked == 0, "without hw.sensors.gyro: %u services, the sample read %u times",
	    service_count, kernel_sample.asked);
	kernel_says(&kernel_gyro, 1, one);
	kernel_says(&kernel_motion, 1, off);
	kernel_gyro.asked = 0;
	gyro_thread(&the_client);
	CHECK(service_count == 0 && kernel_sample.asked == 0 && kernel_gyro.asked == 0, "motion_hid=0: %u services, the sample read "
	    "%u times, hw.sensors.gyro asked %u times", service_count, kernel_sample.asked, kernel_gyro.asked);
	kernel_says(&kernel_gyro, 1, off);
	kernel_says(&kernel_motion, 4, motion_goes);
	kernel_gyro.asked = 0;
	gyro_thread(&the_client);
	CHECK(service_count == 0 && kernel_sample.asked == 0 && kernel_gyro.asked <= 4, "the samples taken from the motion stack "
	    "while waited for: %u services, the sample read %u times, hw.sensors.gyro asked %u times", service_count,
	    kernel_sample.asked, kernel_gyro.asked);
}

/* The kernel does not follow the sensor for Darwin (hw.sensors.hid never 1): waited for, then nothing. */
static void
scenario_gyro_waits_for_the_kernel(void)
{
	static const int never[] = { 0 }, one[] = { 1 };

	kernel_says(&kernel_hid, 1, never);
	kernel_says(&kernel_motion, 1, one);
	kernel_says(&kernel_gyro, 1, one);
	gyro_thread(&the_client);
	CHECK(kernel_hid.asked == ORIENTATION_WAIT_S + 1, "asked %u times, not %d", kernel_hid.asked, ORIENTATION_WAIT_S + 1);
	CHECK(service_count == 0 && kernel_sample.asked == 0, "a service made, or a sample read, without the kernel");
}

/* A Darwin whose IOKit has not got IOHIDEventCreateGyroEvent: no gyroscope service, nothing sent, nothing read. */
static void
scenario_gyro_without_the_call(void)
{
	static const int one[] = { 1, 1, 0 };
	static const char *const texts[] = { "1 5000000 0 -1000 0 1222 -40 3" };

	gyro_call_missing = true;
	kernel_says(&kernel_hid, 3, one);
	kernel_says(&kernel_motion, 3, one);
	kernel_says(&kernel_gyro, 3, one);
	samples_say(1, texts);
	gyro_thread(&the_client);
	CHECK(service_count == 0 && sent_count == 0 && kernel_sample.asked == 0, "%u services, %u events, the sample read %u times",
	    service_count, sent_count, kernel_sample.asked);
}

static const struct {
	const char *name;
	void (*run)(void);
} scenarios[] = {
	{ "fingers", scenario_fingers },
	{ "interrupted", scenario_interrupted },
	{ "read_fails", scenario_read_fails },
	{ "busy", scenario_busy },
	{ "orientation_followed", scenario_orientation_followed },
	{ "orientation_said_again", scenario_orientation_said_again },
	{ "orientation_waits_for_the_kernel", scenario_orientation_waits_for_the_kernel },
	{ "orientation_without_the_calls", scenario_orientation_without_the_calls },
	{ "orientation_not_without_the_kernel", scenario_orientation_not_without_the_kernel },
	{ "motion_followed", scenario_motion_followed },
	{ "motion_as_asked", scenario_motion_as_asked },
	{ "motion_feed", scenario_motion_feed },
	{ "motion_kept_from_darwin", scenario_motion_kept_from_darwin },
	{ "motion_waits_for_the_kernel", scenario_motion_waits_for_the_kernel },
	{ "motion_without_the_call", scenario_motion_without_the_call },
	{ "motion_reads_past_the_rates", scenario_motion_reads_past_the_rates },
	{ "gyro_followed", scenario_gyro_followed },
	{ "gyro_as_asked", scenario_gyro_as_asked },
	{ "gyro_kept_from_darwin", scenario_gyro_kept_from_darwin },
	{ "gyro_waits_for_the_kernel", scenario_gyro_waits_for_the_kernel },
	{ "gyro_without_the_call", scenario_gyro_without_the_call },
};

int
main(int argc, char **argv)
{
	(void)input_start;
	(void)dispatch_semaphore_create;
	(void)test_close;              /* (a library that never closes the device) */
	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			printf("%s: %s\n", scenarios[i].name, failures ? "FAILED" : "ok");
			return failures != 0;
		}
	}
	fprintf(stderr, "%s: no scenario %s\n", TEST_NAME, argv[1]);
	return 2;
}
