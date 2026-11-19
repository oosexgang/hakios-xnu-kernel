/*
 * Host tests of the touch screen and the buttons of Darwin 26's backboardd (userland/userland/src/inject26.c): its
 * part between "---- backboardd: the touch screen and the buttons" and "---- big thread stacks", which
 * workbench/run.sh cuts out of the file unchanged as inject26_input.c (the rest of the file is Objective-C
 * runtime calls and blocks, which have nothing to do with touches and which the host's compiler cannot build).
 * What it is given of CoreFoundation, IOKit's HID event calls, dispatch, notify and the Mach clock is this
 * file's: the events it makes are kept and looked at, and counted, so that one not let go is seen; and so are
 * its open(), read() and close() of /dev/hakios-touch, which play a script of samples and errors.
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
#define kCFStringEncodingUTF8   0x08000100
static const int kCFTypeSetCallBacks, kCFTypeDictionaryKeyCallBacks, kCFTypeDictionaryValueCallBacks, kCFTypeArrayCallBacks;
static const char cf_true[] = "true", cf_object[] = "a CoreFoundation object";
#define kCFBooleanTrue          ((CFBooleanRef)cf_true)

static CFNumberRef CFNumberCreate(CFAllocatorRef a, int type, const void *value) { (void)a; (void)type; (void)value; return cf_object; }
static CFMutableDictionaryRef
CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex n, const void *k, const void *v)
{
	(void)a; (void)n; (void)k; (void)v;
	return (CFMutableDictionaryRef)cf_object;
}
static void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v) { (void)d; (void)k; (void)v; }
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
static CFTypeID CFGetTypeID(CFTypeRef o) { (void)o; return 0; }
static CFTypeID CFBooleanGetTypeID(void) { return 1; }
static CFTypeID CFNumberGetTypeID(void) { return 2; }
static Boolean CFBooleanGetValue(CFBooleanRef b) { return b == kCFBooleanTrue; }
static Boolean CFNumberGetValue(CFNumberRef n, int type, void *value) { (void)n; (void)type; *(int *)value = 0; return true; }
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
static int sysctlbyname(const char *n, void *o, size_t *s, void *i, size_t is) { (void)n; (void)o; (void)s; (void)i; (void)is; return -1; }

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
	uint32_t mask, index, usage;
	double x, y;
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
	(void)client; (void)attributes; (void)callbacks; (void)target;
	((struct service *)refcon)->opened = true;     /* (the event server takes it at once) */
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
	(void)event; (void)field; (void)value;
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
		if (!sent[i].event.keyboard && n-- == 0) {
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

static const struct {
	const char *name;
	void (*run)(void);
} scenarios[] = {
	{ "fingers", scenario_fingers },
	{ "interrupted", scenario_interrupted },
	{ "read_fails", scenario_read_fails },
	{ "busy", scenario_busy },
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
