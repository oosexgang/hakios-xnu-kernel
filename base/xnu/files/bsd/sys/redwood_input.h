/*
 * /dev/redwood-touch: touch samples on their way from whoever produces them to the
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
#ifndef _SYS_REDWOOD_INPUT_H_
#define _SYS_REDWOOD_INPUT_H_

#include <stdint.h>

#define REDWOOD_TOUCH_DEVICE    "/dev/redwood-touch"

#define REDWOOD_TOUCH_DOWN      1       /* a finger has landed */
#define REDWOOD_TOUCH_MOVE      2
#define REDWOOD_TOUCH_UP        3       /* it has lifted (x, y: where it was last) */
#define REDWOOD_BUTTON_DOWN     4       /* a hardware button; `finger` says which (REDWOOD_BUTTON_*), x and y are 0 */
#define REDWOOD_BUTTON_UP       5

#define REDWOOD_BUTTON_LOCK     1       /* the side button */
#define REDWOOD_BUTTON_VOLUME_UP 2
#define REDWOOD_BUTTON_VOLUME_DOWN 3
#define REDWOOD_BUTTON_HOME     4

struct redwood_touch {
	uint64_t time;                  /* mach_absolute_time() when the sample was posted */
	uint32_t type;                  /* REDWOOD_TOUCH_* or REDWOOD_BUTTON_DOWN/UP */
	uint32_t finger;                /* which finger, from 0 (or which button) */
	uint32_t x, y;                  /* position on the display, 0 = left/top edge, 65535 = right/bottom edge */
};

#ifdef KERNEL
/* For drivers; not from interrupt context. */
void redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
void redwood_input_init(void);
void redwood_virtio_input_init(void);   /* boot-arg virtio_input=1: QEMU's virtio pointer and keyboard */
void redwood_goodix_init(void);         /* boot-arg touchscreen=1: the phone's Goodix or FocalTech touch screen */
void redwood_buttons_init(void);        /* boot-arg buttons=1: the phone's power and volume keys */
void redwood_ufs_init(void);           /* ufs=2 to xnuloader: the phone's storage, read only, through the loader (ufs_shared_pa=) */
void redwood_ufs_report(const char *when); /* where reading the storage has spent its time so far */
bool redwood_ufs_dev(dev_t dev);           /* one of the storage's devices */
void redwood_battery_init(void);        /* boot-arg battery=1: Qualcomm UEFI charger state shared by xnuloader */
int redwood_battery_temperature(void);  /* the battery's temperature in C, -273 while it is not known */
/* one transfer at a time through the PMIC arbiter for this system (bsd/dev/redwood_buttons.c) */
void redwood_spmi_lock(void);
void redwood_spmi_unlock(void);
/*
 * How the battery is, as the audio DSP's battery manager says (bsd/dev/redwood_adsp.c, with adsp=1):
 * handed to bsd/dev/redwood_battery.c, which keeps it for the sysctls kern.redwood.battery_* and
 * passes it on to iokit/Kernel/RedwoodBattery.cpp, the power source that iOS reads. The structure
 * is repeated there (this header is not seen from iokit/), and `size` tells whether the two agree.
 */
struct redwood_battery_state {
	uint32_t size;                  /* sizeof(struct redwood_battery_state) */
	int32_t percent_hundredths;     /* the charge, 0..10000 */
	int32_t voltage_uv;
	int32_t current_ua;             /* more than 0 while the battery takes charge */
	int32_t temperature_hundredths; /* of a degree Celsius */
	int32_t present;                /* 1 if there is a battery */
	int32_t external;               /* 1 if a charger or a computer is on the USB port */
	int32_t status;                 /* Linux's POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
	int32_t charge_uah, full_uah, design_uah, cycles;      /* 0 when not known (yet) */
	int32_t usb_type;               /* Linux's POWER_SUPPLY_USB_TYPE_*: 1 a computer's port, 2 a charger, ... */
	int32_t usb_voltage_uv, usb_current_ua;                 /* what the port gives */
};
void redwood_battery_report(const struct redwood_battery_state *state);
void redwood_net_init(void);            /* en0, if there is something to carry its frames: sys/redwood_net.h */
void redwood_cpufreq_init(void);        /* boot-arg cpufreq=1: the phone's processor at full speed */
void redwood_adreno_init(void);         /* boot-arg adreno=N: the first N steps of waking the phone's GPU */
void redwood_adreno_quiesce(void);      /* on the way back to the loader: the GPU block stopped, out of our memory */
void redwood_adreno_stop(void);         /* before that, where the ending is no panic's: the GPU given back in order */
void redwood_adreno_minute(void);       /* once a minute: a line about how warm the GPU is, if it is in use */
void redwood_wifi_init(void);           /* boot-arg wifi=1: the phone's Wi-Fi; wifi_step=N: the first N steps of it */
void redwood_adsp_init(void);           /* boot-arg adsp=1: the phone's audio DSP started and linked; adsp_step=N: the first N steps */
void redwood_wifi_stop(void);           /* where a run ends in order: the Wi-Fi DSP, if it was started, shut down */
int redwood_wifi_say(const char *format, ...) __printflike(1, 2);      /* a line of the Wi-Fi steps' log */
int redwood_wifi_tell(const char *format, ...) __printflike(1, 2);     /* ... one that is said even with wifi=1 */
uint32_t redwood_wifi_option(const char *name);                         /* a boot-arg's number, or what wifi=1 stands for */
const uint8_t *redwood_wifi_file(const char *name, uint64_t *size);     /* a file of fw/ in the small image, in place */
bool redwood_wlfw_exchange(uint32_t step);                              /* step 15 of wifi_step=N: redwood_wlfw.c */
bool redwood_wlfw_registers(uint64_t *address, uint32_t *size, bool *mission); /* ... and what it learnt for step 16 */
void redwood_wlfw_stop(uint32_t step);                                  /* ... and its end, before the DSP is stopped */
void redwood_wifi_quiesce(void);        /* on the way back to the loader: the Wi-Fi DSP, if it still runs, shut down */
/* ath11k's object and the kernel's side of it (bsd/dev/redwood_lx.c), and what that asks of the phone's radio */
int redwood_lx_start(void);             /* the driver brought up on the radio as far as it is built: 0 or an errno */
void redwood_lx_stop(void);             /* ... and down again, if it stayed up (a network joined) */
int redwood_lx_prepare(void);           /* before all that: the memory the radio will be shown, taken; 0 or an errno */
void redwood_lx_net_early(void);        /* ... and en0 made on the radio now, if the kernel was given its address */
bool redwood_wifi_dsp_is_up(void);      /* the DSP that runs the radio's firmware has been started and has a link */
const uint8_t *redwood_wifi_driver_file(const char *name, uint64_t *size);     /* a file by the name Linux has for it */
bool redwood_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address); /* memory shown to the radio */
bool redwood_wifi_radio_msi(unsigned long long *address, uint32_t *data, uint32_t *first_spi); /* where the radio raises its interrupts */
bool redwood_wifi_radio_address(uint8_t *address);      /* the radio's own address (boot-arg wifi_mac), if one was given */
/* the network to join (fw/network of the small image): name and passphrase, buffer sizes in and lengths out */
bool redwood_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length);

/*
 * The GPU kept on (boot-arg adreno_keep=1), for iokit/Kernel/RedwoodGPU.cpp: see "the GPU kept on"
 * in bsd/dev/redwood_adreno.c. One caller at a time.
 */
bool redwood_gpu_ready(void);
bool redwood_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t redwood_gpu_address(uint64_t size);
void redwood_gpu_address_free(uint64_t address, uint64_t size);
void redwood_gpu_space(uint64_t *used, uint32_t *live, uint64_t *largest, unsigned *holes, unsigned *most,
    uint64_t *lost, uint32_t *refused);
extern uint32_t gpu_va;                 /* boot-arg gpu_va: 0 as before, 1 describe the addresses, 2 also guard them */
bool redwood_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void redwood_gpu_mapped(void);
void redwood_gpu_unmap(uint64_t address, uint64_t size);
void redwood_gpu_to_memory(void *start, uint64_t size);
uint32_t redwood_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool redwood_gpu_done(uint32_t fence);
uint32_t redwood_gpu_last(void);
void redwood_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
void redwood_gpu_report(const char *why);
bool redwood_gpu_recover(void);
void redwood_gpu_stop(void);
#endif

#endif /* _SYS_REDWOOD_INPUT_H_ */
