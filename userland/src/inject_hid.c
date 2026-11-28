/*
 * libhakios.hid.dylib, inside BTLEServer: a Bluetooth LE keyboard's keys to backboardd.
 *
 * BTLEServer reads an LE keyboard's HID reports over GATT and hands them to an IOHIDUserDevice, a HID device made
 * from user space through IOHIDFamily's IOHIDResource. This port has no IOHIDFamily, so IOHIDUserDeviceCreateWithOptions
 * fails ("[userdevice] IOHIDResource not found", "Error creating HID device", log 233933) and the keys go nowhere.
 * Here the IOHIDUserDevice calls BTLEServer makes are taken over (dyld's interposing): when IOKit cannot make the
 * device, a stand-in is returned, the device's report descriptor is read for its keyboard fields (usage page 7), and
 * each report is turned into keys going down and up, written to /dev/hakios-touch as HAKIOS_KEY_DOWN/UP. Our code in
 * backboardd (src/inject.c, send_key()) makes them keyboard events. When BTLEServer lets the device go (it takes it
 * off its dispatch queue), every key is let up and HAKIOS_KEYBOARD_GONE is written.
 *
 * Only the keyboard page is followed: a keyboard's media keys (consumer page) and a mouse are not yet.
 *
 * It is in bluetoothd as well, which makes an IOHIDUserDevice for a Classic HID device, and a speaker or headphones
 * with buttons are one: when that failed, bluetoothd gave up the device's HID channel ("Handshake failed to complete
 * ... disconnecting") and the speaker hung up the whole link, its audio with it (log 002408). There the stand-in
 * keeps the link up; a device without keyboard fields has its reports taken and dropped.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hakios.input.h"

/* IOKit/hid/IOHIDUserDevice.h, which the iPhoneOS SDK does not carry */
typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;
typedef int IOReturn;
typedef uint32_t IOOptionBits;
typedef mach_port_t io_service_t;
typedef IOReturn (*IOHIDUserDeviceReportCallback)(void *refcon, int type, uint32_t reportID, uint8_t *report,
    CFIndex reportLength);
typedef IOReturn (*IOHIDUserDeviceReportWithReturnLengthCallback)(void *refcon, int type, uint32_t reportID,
    uint8_t *report, CFIndex *reportLength);

extern IOHIDUserDeviceRef IOHIDUserDeviceCreateWithOptions(CFAllocatorRef allocator, CFDictionaryRef properties,
    IOOptionBits options);
extern void IOHIDUserDeviceScheduleWithDispatchQueue(IOHIDUserDeviceRef device, dispatch_queue_t queue);
extern void IOHIDUserDeviceUnscheduleFromDispatchQueue(IOHIDUserDeviceRef device, dispatch_queue_t queue);
extern void IOHIDUserDeviceRegisterSetReportCallback(IOHIDUserDeviceRef device, IOHIDUserDeviceReportCallback callback,
    void *refcon);
extern void IOHIDUserDeviceRegisterGetReportWithReturnLengthCallback(IOHIDUserDeviceRef device,
    IOHIDUserDeviceReportWithReturnLengthCallback callback, void *refcon);
extern IOReturn IOHIDUserDeviceHandleReport(IOHIDUserDeviceRef device, const uint8_t *report, CFIndex reportLength);
extern IOReturn IOHIDUserDeviceHandleReportWithTimeStamp(IOHIDUserDeviceRef device, uint64_t timestamp,
    const uint8_t *report, CFIndex reportLength);
extern io_service_t IOHIDUserDeviceCopyService(IOHIDUserDeviceRef device);

#define HID_PAGE_KEYBOARD       0x07
#define MAX_FIELDS              16
#define MAX_USAGES              16
#define MAX_IDS                 4

/* One Input item of the keyboard page: where its bits are in its report and what they mean. */
struct field {
	uint8_t report_id;
	uint16_t bit_offset;            /* from the first byte after the report ID, if there are IDs */
	uint8_t size, count;
	bool variable;                  /* one bit (or value) per usage, as the modifiers are; else an array of key codes */
	int32_t logical_min;
	uint16_t usage_min;             /* a range of usages, or ... */
	uint16_t usages[MAX_USAGES];    /* ... a list (usage_count of them) */
	uint8_t usage_count;
};

/* A stand-in device: its fields, and the keys each of its keyboard reports has down. */
struct device {
	struct field fields[MAX_FIELDS];
	unsigned field_count;
	bool ids;                       /* the reports start with a report ID */
	uint8_t id_of[MAX_IDS];
	uint8_t down[MAX_IDS][32];      /* per report ID: a bit per usage 0..255 */
	unsigned id_count, reports;
	bool gone;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableSetRef stand_ins;       /* the CFData objects handed out as devices; their bytes are the struct device */
static int input = -1;
static unsigned keys_told, keyboards;     /* keyboards: stand-ins with keyboard fields now */

static void
say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	fprintf(stderr, "hakios: hid: ");
	vfprintf(stderr, format, arguments);
	va_end(arguments);
}

static struct device *
stand_in(IOHIDUserDeviceRef device)
{
	struct device *d = NULL;

	pthread_mutex_lock(&lock);
	if (device != NULL && stand_ins != NULL && CFSetContainsValue(stand_ins, device)) {
		d = (struct device *)CFDataGetMutableBytePtr((CFMutableDataRef)device);
	}
	pthread_mutex_unlock(&lock);
	return d;
}

static void
post(uint32_t type, uint32_t usage, uint32_t page)
{
	struct hakios_touch t = { .type = type, .finger = usage, .x = page };

	if (input < 0) {
		input = open(HAKIOS_TOUCH_DEVICE, O_WRONLY);
		if (input < 0) {
			say("%s: cannot open it for writing\n", HAKIOS_TOUCH_DEVICE);
			return;
		}
	}
	if (write(input, &t, sizeof(t)) != (ssize_t)sizeof(t)) {
		say("%s: a key could not be written\n", HAKIOS_TOUCH_DEVICE);
	}
}

/* The descriptor's Input items on the keyboard page (HID 1.11, 6.2.2: short items only). */
static void
parse(struct device *d, const uint8_t *p, CFIndex length)
{
	uint32_t page = 0, size = 0, count = 0, id = 0;
	int32_t logical_min = 0;
	uint32_t usage_min = 0, usage_max = 0;
	uint16_t usages[MAX_USAGES];
	unsigned usage_count = 0;
	uint16_t offset[256] = { 0 };
	CFIndex i = 0;

	while (i < length) {
		uint8_t prefix = p[i++];
		uint32_t n = (prefix & 3) == 3 ? 4 : prefix & 3, value = 0;
		int32_t signed_value;

		if (prefix == 0xfe) {           /* a long item: skipped */
			if (i + 1 < length) {
				i += 2 + p[i];
			}
			continue;
		}
		if (i + (CFIndex)n > length) {
			break;
		}
		for (uint32_t k = 0; k < n; k++) {
			value |= (uint32_t)p[i + k] << (8 * k);
		}
		signed_value = n == 1 ? (int8_t)value : n == 2 ? (int16_t)value : (int32_t)value;
		i += n;
		switch (prefix & 0xfc) {
		case 0x04: page = value; break;                 /* Usage Page */
		case 0x14: logical_min = signed_value; break;   /* Logical Minimum */
		case 0x74: size = value; break;                 /* Report Size */
		case 0x94: count = value; break;                /* Report Count */
		case 0x84: id = value & 0xff; d->ids = true; break;     /* Report ID */
		case 0x08:                                      /* Usage */
			if (usage_count < MAX_USAGES) {
				usages[usage_count++] = (uint16_t)value;
			}
			break;
		case 0x18: usage_min = value; break;            /* Usage Minimum */
		case 0x28: usage_max = value; break;            /* Usage Maximum */
		case 0x80:                                      /* Input */
		case 0x90:                                      /* Output */
		case 0xb0: {                                    /* Feature */
			bool input_item = (prefix & 0xfc) == 0x80;

			if (input_item && page == HID_PAGE_KEYBOARD && !(value & 1) && d->field_count < MAX_FIELDS &&
			    size > 0 && size <= 16 && count > 0 && count <= 255) {
				struct field *f = &d->fields[d->field_count++];

				f->report_id = (uint8_t)id;
				f->bit_offset = offset[id];
				f->size = (uint8_t)size;
				f->count = (uint8_t)count;
				f->variable = (value & 2) != 0;
				f->logical_min = logical_min;
				f->usage_min = (uint16_t)(usage_count == 0 ? usage_min : 0);
				f->usage_count = (uint8_t)usage_count;
				memcpy(f->usages, usages, usage_count * sizeof(usages[0]));
				(void)usage_max;
			}
			if (input_item) {
				offset[id] += (uint16_t)(size * count);
			}
			usage_count = 0, usage_min = usage_max = 0;
			break;
		}
		case 0xa0: case 0xc0:                           /* Collection, End Collection: the locals end */
			usage_count = 0, usage_min = usage_max = 0;
			break;
		default:
			break;
		}
	}
	for (unsigned k = 0; k < d->field_count; k++) {
		say("keyboard field %u: report %u, bit %u, %u x %u bits, %s\n", k, d->fields[k].report_id,
		    d->fields[k].bit_offset, d->fields[k].count, d->fields[k].size, d->fields[k].variable ? "one per key" :
		    "key codes");
	}
}

static uint32_t
bits(const uint8_t *data, CFIndex length, uint32_t at, uint32_t size)
{
	uint32_t value = 0;

	for (uint32_t b = 0; b < size; b++) {
		uint32_t bit = at + b;

		if ((CFIndex)(bit / 8) < length && (data[bit / 8] >> (bit % 8) & 1)) {
			value |= 1u << b;
		}
	}
	return value;
}

static int
slot(struct device *d, uint8_t id)
{
	for (unsigned k = 0; k < d->id_count; k++) {
		if (d->id_of[k] == id) {
			return (int)k;
		}
	}
	if (d->id_count == MAX_IDS) {
		return -1;
	}
	d->id_of[d->id_count] = id;
	return (int)d->id_count++;
}

/* A report: the keys its fields have down now, against those they had down before. */
static void
report(struct device *d, const uint8_t *r, CFIndex length)
{
	uint8_t id = 0, now[32] = { 0 };
	bool any = false;
	int s;

	if (d->ids) {
		if (length < 1) {
			return;
		}
		id = r[0], r++, length--;
	}
	for (unsigned k = 0; k < d->field_count; k++) {
		const struct field *f = &d->fields[k];

		if (f->report_id != id) {
			continue;
		}
		any = true;
		for (uint32_t j = 0; j < f->count; j++) {
			uint32_t v = bits(r, length, f->bit_offset + j * f->size, f->size), usage;

			if (f->variable) {
				if (v == 0) {
					continue;
				}
				usage = f->usage_count != 0 ? (j < f->usage_count ? f->usages[j] : 0) : f->usage_min + j;
			} else {
				int32_t index = (int32_t)v - f->logical_min;

				if (index < 0) {
					continue;
				}
				usage = f->usage_count != 0 ? ((uint32_t)index < f->usage_count ? f->usages[index] : 0) :
				    f->usage_min + (uint32_t)index;
				if (usage == 0x01 || usage == 0x02 || usage == 0x03) {
					return;         /* ErrorRollOver, POSTFail, ErrorUndefined: this report says nothing */
				}
			}
			if (usage > 0 && usage < 256) {
				now[usage / 8] |= 1 << (usage % 8);
			}
		}
	}
	if (!any || (s = slot(d, id)) < 0) {
		static unsigned others;

		if (others++ < 12) {
			char hex[3 * 12 + 1] = "";

			for (CFIndex k = 0, n = 0; k < length && k < 12; k++) {
				n += snprintf(hex + n, sizeof(hex) - (size_t)n, " %02x", r[k]);
			}
			say("report %u (%ld bytes:%s) has no keyboard field: not followed\n", id, (long)length, hex);
		}
		return;
	}
	if (memcmp(d->down[s], now, sizeof(now)) == 0) {
		static unsigned same;
		bool empty = true;

		for (CFIndex k = 0; k < length; k++) {
			empty &= r[k] == 0;
		}
		if (!empty && same++ < 12) {
			char hex[3 * 12 + 1] = "";

			for (CFIndex k = 0, n = 0; k < length && k < 12; k++) {
				n += snprintf(hex + n, sizeof(hex) - (size_t)n, " %02x", r[k]);
			}
			say("report %u (%ld bytes:%s) changes no key\n", id, (long)length, hex);
		}
	}
	for (unsigned u = 4; u < 256; u++) {
		bool was = d->down[s][u / 8] >> (u % 8) & 1, is = now[u / 8] >> (u % 8) & 1;

		if (was != is) {
			post(is ? HAKIOS_KEY_DOWN : HAKIOS_KEY_UP, u, HID_PAGE_KEYBOARD);
			/* said: the first keys, and after them only keys that are no letter or digit (0x28 Enter on) */
			if (keys_told++ < 4 || (u >= 0x28 && keys_told < 400)) {
				say("key %#x %s\n", u, is ? "down" : "up");
			}
		}
	}
	memcpy(d->down[s], now, sizeof(now));
}

static IOHIDUserDeviceRef
hakios_create(CFAllocatorRef allocator, CFDictionaryRef properties, IOOptionBits options)
{
	IOHIDUserDeviceRef device = IOHIDUserDeviceCreateWithOptions(allocator, properties, options);
	CFMutableDataRef made;
	CFDataRef descriptor;
	struct device *d;

	if (device != NULL || properties == NULL) {
		return device;
	}
	descriptor = CFDictionaryGetValue(properties, CFSTR("ReportDescriptor"));
	if (descriptor == NULL || CFGetTypeID(descriptor) != CFDataGetTypeID()) {
		say("IOKit made no device and it has no report descriptor: none made\n");
		return NULL;
	}
	made = CFDataCreateMutable(NULL, sizeof(struct device));
	CFDataSetLength(made, sizeof(struct device));
	d = (struct device *)CFDataGetMutableBytePtr(made);
	memset(d, 0, sizeof(*d));
	parse(d, CFDataGetBytePtr(descriptor), CFDataGetLength(descriptor));
	say("IOKit made no device (no IOHIDResource): a stand-in %p with %u keyboard fields, %ld bytes of descriptor\n",
	    (void *)made, d->field_count, (long)CFDataGetLength(descriptor));
	pthread_mutex_lock(&lock);
	keyboards += d->field_count != 0;
	if (stand_ins == NULL) {
		stand_ins = CFSetCreateMutable(NULL, 0, NULL);  /* not retained: a released stand-in is told by the queue */
	}
	CFSetAddValue(stand_ins, made);
	pthread_mutex_unlock(&lock);
	return (IOHIDUserDeviceRef)made;
}

static void
hakios_schedule(IOHIDUserDeviceRef device, dispatch_queue_t queue)
{
	if (stand_in(device) == NULL) {
		IOHIDUserDeviceScheduleWithDispatchQueue(device, queue);
	}
}

static void
hakios_unschedule(IOHIDUserDeviceRef device, dispatch_queue_t queue)
{
	struct device *d = stand_in(device);

	if (d == NULL) {
		IOHIDUserDeviceUnscheduleFromDispatchQueue(device, queue);
		return;
	}
	for (unsigned s = 0; s < d->id_count; s++) {
		for (unsigned u = 4; u < 256; u++) {
			if (d->down[s][u / 8] >> (u % 8) & 1) {
				post(HAKIOS_KEY_UP, u, HID_PAGE_KEYBOARD);
			}
		}
	}
	memset(d->down, 0, sizeof(d->down));
	say("stand-in %p let go after %u reports\n", (void *)device, d->reports);
	pthread_mutex_lock(&lock);
	CFSetRemoveValue(stand_ins, device);
	/* the keyboard service goes when the last keyboard does, not when a speaker's buttons or a game pad go */
	bool last = d->field_count != 0 && keyboards > 0 && --keyboards == 0;
	pthread_mutex_unlock(&lock);
	if (last) {
		post(HAKIOS_KEYBOARD_GONE, 0, 0);
	}
}

static void
hakios_set_report_callback(IOHIDUserDeviceRef device, IOHIDUserDeviceReportCallback callback, void *refcon)
{
	if (stand_in(device) == NULL) {
		IOHIDUserDeviceRegisterSetReportCallback(device, callback, refcon);
	}
}

static void
hakios_get_report_callback(IOHIDUserDeviceRef device, IOHIDUserDeviceReportWithReturnLengthCallback callback,
    void *refcon)
{
	if (stand_in(device) == NULL) {
		IOHIDUserDeviceRegisterGetReportWithReturnLengthCallback(device, callback, refcon);
	}
}

static IOReturn
hakios_handle_report(IOHIDUserDeviceRef device, const uint8_t *r, CFIndex length)
{
	struct device *d = stand_in(device);

	if (d == NULL) {
		return IOHIDUserDeviceHandleReport(device, r, length);
	}
	d->reports++;
	report(d, r, length);
	return 0;
}

static IOReturn
hakios_handle_report_with_time_stamp(IOHIDUserDeviceRef device, uint64_t timestamp, const uint8_t *r, CFIndex length)
{
	struct device *d = stand_in(device);

	if (d == NULL) {
		return IOHIDUserDeviceHandleReportWithTimeStamp(device, timestamp, r, length);
	}
	d->reports++;
	report(d, r, length);
	return 0;
}

static io_service_t
hakios_copy_service(IOHIDUserDeviceRef device)
{
	return stand_in(device) == NULL ? IOHIDUserDeviceCopyService(device) : MACH_PORT_NULL;
}

#define INTERPOSE(replacement, original) \
	{ (const void *)(unsigned long)&replacement, (const void *)(unsigned long)&original }

__attribute__((used)) static const struct {
	const void *replacement, *original;
} interposed[] __attribute__((section("__DATA,__interpose"))) = {
	INTERPOSE(hakios_create, IOHIDUserDeviceCreateWithOptions),
	INTERPOSE(hakios_schedule, IOHIDUserDeviceScheduleWithDispatchQueue),
	INTERPOSE(hakios_unschedule, IOHIDUserDeviceUnscheduleFromDispatchQueue),
	INTERPOSE(hakios_set_report_callback, IOHIDUserDeviceRegisterSetReportCallback),
	INTERPOSE(hakios_get_report_callback, IOHIDUserDeviceRegisterGetReportWithReturnLengthCallback),
	INTERPOSE(hakios_handle_report, IOHIDUserDeviceHandleReport),
	INTERPOSE(hakios_handle_report_with_time_stamp, IOHIDUserDeviceHandleReportWithTimeStamp),
	INTERPOSE(hakios_copy_service, IOHIDUserDeviceCopyService),
};
