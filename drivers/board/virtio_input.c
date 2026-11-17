/*
 * Virtio input devices as a source of touches and buttons (see sys/board/input.h).
 *
 * Under QEMU's "virt" machine a "virtio-tablet-device" stands for the touch screen (the pointer
 * with its button held is a finger) and a "virtio-keyboard-device" for the phone's buttons. They
 * sit on the machine's virtio-mmio bus, which must be the current one, not "legacy"
 * (-global virtio-mmio.force-legacy=false). Only with boot-arg virtio_input=1: the bus address
 * is QEMU's and means nothing on the phone.
 *
 * No interrupt is used; the event queues are looked at 125 times a second.
 *
 * The keyboard is also a way to tell the kernel something from the host while the system runs:
 * F12, a process name, Enter prints where every thread of that process is (ustacks_now(); the
 * host side is "qemu/touch.py stacks NAME"). F11 is five fingers on the screen for as long as it
 * is held, which is how the way out in board/input.c is tried ("qemu/touch.py key fivefingers 2000").
 */
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/board/input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>  /* xnu-12377: the KMA flags and kmem_alloc moved to the internal header */

#include "kernel.h"
#include "virtio.h"

#define QUEUE_SIZE              64

struct vq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};
struct vq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[QUEUE_SIZE];
};
struct vq_used {
	uint16_t flags;
	uint16_t idx;
	struct {
		uint32_t id;
		uint32_t len;
	} ring[QUEUE_SIZE];
};
struct input_event {
	uint16_t type;
	uint16_t code;
	uint32_t value;
};

/* everything the device reads and writes is in one page */
#define OFFSET_DESC             0x0000
#define OFFSET_AVAIL            0x0400
#define OFFSET_USED             0x0800
#define OFFSET_EVENTS           0x1000

/* Linux input event numbers, which is what virtio input speaks */
#define EV_SYN                  0
#define EV_KEY                  1
#define EV_ABS                  3
#define ABS_X                   0
#define ABS_Y                   1
#define ABS_RANGE               0x7fff          /* QEMU's tablet: 0..0x7fff across the screen */
#define BTN_LEFT                0x110
#define BTN_TOUCH               0x14a

struct input_device {
	vm_offset_t             regs;
	struct vq_avail *       avail;
	struct vq_used *        used;
	struct input_event *    events;
	uint16_t                seen;           /* how far the used ring has been read */
	/* the pointer */
	uint32_t                x, y;
	bool                    pressed, touching, moved;
};

#define MAX_DEVICES     4
static struct input_device devices[MAX_DEVICES];
static int device_count;
static thread_call_t poll_call;

static inline uint32_t
reg_read(struct input_device *d, uint32_t reg)
{
	return *(volatile uint32_t *)(d->regs + reg);
}

static inline void
reg_write(struct input_device *d, uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(d->regs + reg) = value;
}

static inline void
reg_write64(struct input_device *d, uint32_t reg, uint64_t value)
{
	reg_write(d, reg, (uint32_t)value);
	reg_write(d, reg + 4, (uint32_t)(value >> 32));
}

extern void ustacks_now(const char *name);

#define KEY_ENTER               28
#define KEY_LEFTSHIFT           42
#define KEY_RIGHTSHIFT          54
#define KEY_F11                 87
#define KEY_F12                 88

/* F12 NAME Enter: the keys in between spell a process name. Returns true if the key was taken. */
static bool
command_key(uint16_t key, uint32_t value)
{
	static const char plain[] =
	    "\0\0001234567890-=\0\0qwertyuiop[]\0\0asdfghjkl;'`\0\\zxcvbnm,./\0\0\0 ";
	static char name[32];
	static unsigned length;
	static bool active, shift;

	if (key == KEY_F12) {
		if (value == 1) {
			active = true;
			length = 0;
		}
		return true;
	}
	if (!active) {
		return false;
	}
	if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) {
		shift = value != 0;
	} else if (value != 1) {
		/* a release or a repeat */
	} else if (key == KEY_ENTER) {
		active = false;
		name[length] = 0;
		if (length) {
			printf("virtio-input: stacks of \"%s\" asked for\n", name);
			ustacks_now(name);
		}
	} else if (key < sizeof(plain) - 1 && plain[key] != 0 && length < sizeof(name) - 1) {
		char c = plain[key];

		if (shift && c >= 'a' && c <= 'z') {
			c -= 'a' - 'A';
		} else if (shift && c == '-') {
			c = '_';
		}
		name[length++] = c;
	}
	return true;
}

/* Which of the phone's buttons a key stands for; 0 = none. */
static uint32_t
button_for_key(uint16_t key)
{
	switch (key) {
	case 116: /* KEY_POWER */
	case 38:  /* KEY_L */
		return HAKIOS_BUTTON_LOCK;
	case 115: /* KEY_VOLUMEUP */
	case 103: /* KEY_UP */
		return HAKIOS_BUTTON_VOLUME_UP;
	case 114: /* KEY_VOLUMEDOWN */
	case 108: /* KEY_DOWN */
		return HAKIOS_BUTTON_VOLUME_DOWN;
	case 102: /* KEY_HOME */
	case 35:  /* KEY_H */
		return HAKIOS_BUTTON_HOME;
	}
	return 0;
}

static void
input_event(struct input_device *d, const struct input_event *e)
{
	uint32_t button;

	switch (e->type) {
	case EV_ABS:
		if (e->code == ABS_X) {
			d->x = e->value * 65535 / ABS_RANGE;
			d->moved = true;
		} else if (e->code == ABS_Y) {
			d->y = e->value * 65535 / ABS_RANGE;
			d->moved = true;
		}
		break;
	case EV_KEY:
		if (e->code == BTN_LEFT || e->code == BTN_TOUCH) {
			d->pressed = e->value != 0;
		} else if (command_key(e->code, e->value)) {
			break;
		} else if (e->code == KEY_F11 && e->value < 2) {
			/* five more fingers, for as long as the key is held: the way out of board/input.c */
			for (uint32_t i = 0; i < 5; i++) {
				hakios_touch_post(e->value ? HAKIOS_TOUCH_DOWN : HAKIOS_TOUCH_UP, 5 + i, 13107 + 9830 * i, 32768);
			}
		} else if ((button = button_for_key(e->code)) != 0 && e->value < 2) {   /* 2 = key repeat */
			hakios_touch_post(e->value ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, button, 0, 0);
		}
		break;
	case EV_SYN:
		if (d->pressed && !d->touching) {
			hakios_touch_post(HAKIOS_TOUCH_DOWN, 0, d->x, d->y);
		} else if (d->pressed && d->moved) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, 0, d->x, d->y);
		} else if (!d->pressed && d->touching) {
			hakios_touch_post(HAKIOS_TOUCH_UP, 0, d->x, d->y);
		}
		d->touching = d->pressed;
		d->moved = false;
		break;
	}
}

static void
input_poll(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	for (int i = 0; i < device_count; i++) {
		struct input_device *d = &devices[i];
		bool returned = false;

		while (d->seen != *(volatile uint16_t *)&d->used->idx) {
			uint32_t id;

			VIRTIO_USED_READ_BARRIER();
			id = d->used->ring[d->seen++ % QUEUE_SIZE].id % QUEUE_SIZE;

			input_event(d, &d->events[id]);
			/* give the buffer back */
			d->avail->ring[d->avail->idx % QUEUE_SIZE] = (uint16_t)id;
			__builtin_arm_dmb(0xb);         /* ISH */
			d->avail->idx++;
			returned = true;
		}
		if (returned) {
			__builtin_arm_dmb(0xb);
			reg_write(d, VIRTIO_QUEUE_NOTIFY, 0);
		}
	}
	clock_interval_to_deadline(8, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(poll_call, deadline);
}

static bool
input_attach(struct input_device *d, int slot)
{
	vm_offset_t page = 0;
	struct vq_desc *desc;
	uint64_t phys;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	phys = kvtophys(page);
	desc = (struct vq_desc *)(page + OFFSET_DESC);
	d->avail = (struct vq_avail *)(page + OFFSET_AVAIL);
	d->used = (struct vq_used *)(page + OFFSET_USED);
	d->events = (struct input_event *)(page + OFFSET_EVENTS);

	reg_write(d, VIRTIO_STATUS, 0);         /* reset */
	reg_write(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE);
	reg_write(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
	reg_write(d, VIRTIO_DRIVER_FEATURES_SEL, 0);
	reg_write(d, VIRTIO_DRIVER_FEATURES, 0);
	reg_write(d, VIRTIO_DRIVER_FEATURES_SEL, 1);
	reg_write(d, VIRTIO_DRIVER_FEATURES, 1);        /* bit 32: VIRTIO_F_VERSION_1 */
	reg_write(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
	if (!(reg_read(d, VIRTIO_STATUS) & STATUS_FEATURES_OK)) {
		printf("virtio-input: slot %d: features refused\n", slot);
		return false;
	}

	/* queue 0 brings the events: hand the device all the buffers */
	reg_write(d, VIRTIO_QUEUE_SEL, 0);
	if (reg_read(d, VIRTIO_QUEUE_NUM_MAX) < QUEUE_SIZE) {
		printf("virtio-input: slot %d: event queue of %u\n", slot, reg_read(d, VIRTIO_QUEUE_NUM_MAX));
		return false;
	}
	for (int i = 0; i < QUEUE_SIZE; i++) {
		desc[i].addr = phys + OFFSET_EVENTS + (uint64_t)i * sizeof(struct input_event);
		desc[i].len = sizeof(struct input_event);
		desc[i].flags = DESC_F_WRITE;
		d->avail->ring[i] = (uint16_t)i;
	}
	d->avail->flags = AVAIL_F_NO_INTERRUPT;
	d->avail->idx = QUEUE_SIZE;
	reg_write(d, VIRTIO_QUEUE_NUM, QUEUE_SIZE);
	reg_write64(d, VIRTIO_QUEUE_DESC, phys + OFFSET_DESC);
	reg_write64(d, VIRTIO_QUEUE_DRIVER, phys + OFFSET_AVAIL);
	reg_write64(d, VIRTIO_QUEUE_DEVICE, phys + OFFSET_USED);
	__builtin_arm_dmb(0xb);
	reg_write(d, VIRTIO_QUEUE_READY, 1);
	reg_write(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
	reg_write(d, VIRTIO_QUEUE_NOTIFY, 0);
	return true;
}

void
hakios_virtio_input_init(void)
{
	uint32_t enabled = 0;
	vm_offset_t bus;

	if (!PE_parse_boot_argn("virtio_input", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_VIRTIO)) {
		printf("virtio-input: disabled: this board's board.dts lists no \"virtio\" (an emulator's MMIO transport)\n");
		return;
	}
	bus = ml_io_map(VIRTIO_BUS, VIRTIO_SLOTS * VIRTIO_SLOT_SIZE);
	for (int slot = 0; slot < VIRTIO_SLOTS && device_count < MAX_DEVICES; slot++) {
		struct input_device *d = &devices[device_count];

		d->regs = bus + (vm_offset_t)slot * VIRTIO_SLOT_SIZE;
		if (reg_read(d, VIRTIO_MAGIC) != 0x74726976 || reg_read(d, VIRTIO_DEVICE_ID) != VIRTIO_ID_INPUT) {
			continue;
		}
		if (reg_read(d, VIRTIO_VERSION) != 2) {
			printf("virtio-input: slot %d is a legacy transport; start QEMU with -global virtio-mmio.force-legacy=false\n", slot);
			continue;
		}
		if (input_attach(d, slot)) {
			printf("virtio-input: device in slot %d\n", slot);
			device_count++;
		}
	}
	if (device_count) {
		poll_call = thread_call_allocate(input_poll, NULL);
		input_poll(NULL, NULL);
	} else {
		printf("virtio-input: no input devices\n");
	}
}

void
hakios_virtio_input_quiesce(void)
{
	for (int i = 0; i < device_count; i++) {
		reg_write(&devices[i], VIRTIO_STATUS, 0);       /* reset: no more events written into this kernel's buffers */
	}
	device_count = 0;
}
