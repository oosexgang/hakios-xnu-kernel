/*
 * /dev/hakios-touch: touch samples on their way from whoever produces them to the
 * program that turns them into HID events for the window server.
 *
 * iOS expects touches from Apple's multitouch driver through IOHIDFamily; neither exists
 * here. Instead a producer -- a kernel driver for a real touch controller or for an emulated
 * pointer, or a user program replaying a script -- posts samples, and code of ours running
 * inside backboardd reads them and synthesizes the events. Both sides use the same record.
 *
 *   write(): one or more records (time is ignored; the kernel stamps them)
 *   read():  blocks until there is at least one record, returns as many whole ones as fit
 */
#ifndef _SYS_HAKIOS_INPUT_H_
#define _SYS_HAKIOS_INPUT_H_

#include <stdint.h>

#define HAKIOS_TOUCH_DEVICE    "/dev/hakios-touch"

#define HAKIOS_TOUCH_DOWN      1       /* a finger has landed */
#define HAKIOS_TOUCH_MOVE      2
#define HAKIOS_TOUCH_UP        3       /* it has lifted (x, y: where it was last) */
#define HAKIOS_BUTTON_DOWN     4       /* a hardware button; `finger` says which (HAKIOS_BUTTON_*), x and y are 0 */
#define HAKIOS_BUTTON_UP       5

#define HAKIOS_BUTTON_LOCK     1       /* the side button */
#define HAKIOS_BUTTON_VOLUME_UP 2
#define HAKIOS_BUTTON_VOLUME_DOWN 3
#define HAKIOS_BUTTON_HOME     4

struct hakios_touch {
	uint64_t time;                  /* mach_absolute_time() when the sample was posted */
	uint32_t type;                  /* HAKIOS_TOUCH_* or HAKIOS_BUTTON_DOWN/UP */
	uint32_t finger;                /* which finger, from 0 (or which button) */
	uint32_t x, y;                  /* position on the display, 0 = left/top edge, 65535 = right/bottom edge */
};

#ifdef KERNEL
/* For drivers; not from interrupt context. */
void hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
void hakios_input_init(void);
void hakios_virtio_input_init(void);   /* boot-arg virtio_input=1: QEMU's virtio pointer and keyboard */
void hakios_goodix_init(void);         /* boot-arg touchscreen=1: the phone's touch screen */
void hakios_cpufreq_init(void);        /* boot-arg cpufreq=1: the phone's processor at full speed */
#endif

#endif /* _SYS_HAKIOS_INPUT_H_ */
