/*
 * /dev/hakios-touch: touch samples on their way from whoever produces them to the
 * program that turns them into HID events for the window server.
 *
 * Darwin expects touches from Apple's multitouch driver through IOHIDFamily; neither exists
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
void hakios_goodix_init(void);         /* boot-arg touchscreen=1: the phone's Goodix or FocalTech touch screen */
void hakios_buttons_init(void);        /* boot-arg buttons=1: the phone's power and volume keys */
void hakios_ufs_init(void);           /* ufs=2 to xnuloader: the phone's storage, read only, through the loader (ufs_shared_pa=) */
void hakios_ufs_report(const char *when); /* where reading the storage has spent its time so far */
bool hakios_ufs_dev(dev_t dev);           /* one of the storage's devices */
int hakios_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes); /* for drivers */
/* a file of one of the RAM disks (drivers/storage/hakios.image.c): fw/<name> is the firmware userland/build.sh put there */
const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
void hakios_battery_init(void);        /* boot-arg battery=1: Qualcomm UEFI charger state shared by xnuloader */
int hakios_battery_temperature(void);  /* the battery's temperature in C, -273 while it is not known */
/* one transfer at a time through the PMIC arbiter for this system (drivers/input/hakios.buttons.c) */
void hakios_spmi_lock(void);
void hakios_spmi_unlock(void);
/*
 * How the battery is, as the audio DSP's battery manager says (drivers/audio/hakios.adsp.c, with adsp=1):
 * handed to drivers/power/hakios.battery.c, which keeps it for the sysctls kern.hakios.battery_* and
 * passes it on to drivers/power/hakios.battery.cpp, the power source that Darwin reads. The structure
 * is repeated there (this header is not seen from iokit/), and `size` tells whether the two agree.
 */
struct hakios_battery_state {
	uint32_t size;                  /* sizeof(struct hakios_battery_state) */
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
void hakios_battery_report(const struct hakios_battery_state *state);
void hakios_net_init(void);            /* en0, if there is something to carry its frames: sys/hakios.net.h */
void hakios_cpufreq_init(void);        /* boot-arg cpufreq=1: the phone's processor at full speed */
void hakios_adreno_init(void);         /* boot-arg adreno=N: the first N steps of waking the phone's GPU */
void hakios_adreno_quiesce(void);      /* on the way back to the loader: the GPU block stopped, out of our memory */
void hakios_adreno_stop(void);         /* before that, where the ending is no panic's: the GPU given back in order */
void hakios_adreno_minute(void);       /* once a minute: a line about how warm the GPU is, if it is in use */
void hakios_wifi_init(void);           /* boot-arg wifi=1: the phone's Wi-Fi; wifi_step=N: the first N steps of it */
void hakios_qcabt_init(void);          /* boot-arg qcabt=N: the QCA6490's Bluetooth over its UART (mayfly) */
void hakios_adsp_init(void);           /* boot-arg adsp=1: the phone's audio DSP started and linked; adsp_step=N: the first N steps */
void hakios_mpss_init(void);           /* boot-arg modem=1: the phone's modem started and linked; modem_step=N: the first N steps */
void hakios_wifi_stop(void);           /* where a run ends in order: the Wi-Fi DSP, if it was started, shut down */
int hakios_wifi_say(const char *format, ...) __printflike(1, 2);      /* a line of the Wi-Fi steps' log */
int hakios_wifi_tell(const char *format, ...) __printflike(1, 2);     /* ... one that is said even with wifi=1 */
uint32_t hakios_wifi_option(const char *name);                         /* a boot-arg's number, or what wifi=1 stands for */
const uint8_t *hakios_wifi_file(const char *name, uint64_t *size);     /* a file of fw/ in the small image, in place */
bool hakios_wlfw_exchange(uint32_t step);                              /* step 15 of wifi_step=N: hakios.wlfw.c */
bool hakios_wlfw_registers(uint64_t *address, uint32_t *size, bool *mission); /* ... and what it learnt for step 16 */
void hakios_wlfw_stop(uint32_t step);                                  /* ... and its end, before the DSP is stopped */
void hakios_wifi_quiesce(void);        /* on the way back to the loader: the Wi-Fi DSP, if it still runs, shut down */
/* ath11k's object and the kernel's side of it (drivers/wifi/hakios.lx.c), and what that asks of the phone's radio */
int hakios_lx_start(void);             /* the driver brought up on the radio as far as it is built: 0 or an errno */
void hakios_lx_stop(void);             /* ... and down again, if it stayed up (a network joined) */
int hakios_lx_prepare(void);           /* before all that: the memory the radio will be shown, taken; 0 or an errno */
void hakios_lx_net_early(void);        /* ... and en0 made on the radio now, if the kernel was given its address */
bool hakios_wifi_dsp_is_up(void);      /* the DSP that runs the radio's firmware has been started and has a link */
const uint8_t *hakios_wifi_driver_file(const char *name, uint64_t *size);     /* a file by the name Linux has for it */
bool hakios_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address); /* memory shown to the radio */
bool hakios_wifi_radio_msi(unsigned long long *address, uint32_t *data, uint32_t *first_spi); /* where the radio raises its interrupts */
bool hakios_wifi_radio_address(uint8_t *address);      /* the radio's own address (boot-arg wifi_mac), if one was given */
/* the network to join (fw/network of the small image): name and passphrase, buffer sizes in and lengths out */
bool hakios_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length);

/*
 * The GPU kept on (boot-arg adreno_keep=1), for drivers/gpu/hakios.gpu.cpp: see "the GPU kept on"
 * in drivers/gpu/hakios.adreno.c. One caller at a time.
 */
bool hakios_gpu_ready(void);
bool hakios_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t hakios_gpu_address(uint64_t size);
void hakios_gpu_address_free(uint64_t address, uint64_t size);
bool hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void hakios_gpu_mapped(void);
void hakios_gpu_unmap(uint64_t address, uint64_t size);
void hakios_gpu_to_memory(void *start, uint64_t size);
uint32_t hakios_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool hakios_gpu_done(uint32_t fence);
uint32_t hakios_gpu_last(void);
void hakios_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
void hakios_gpu_report(const char *why);
void hakios_gpu_stop(void);
#endif

#endif /* _SYS_HAKIOS_INPUT_H_ */
