/*
 * MediaTek MT6833 (Dimensity 700 / POCO M4 5G) Keypad & Power Button Driver.
 *
 * Provides volume up/down buttons and power/lock key events to Darwin's /dev/hakios-touch.
 *
 * Hardware Mapping (from poco_m4_5g.dts):
 *   Volume Up    : SoC Keypad controller at 0x10010000, key 0 (KP_MEM1 bit 0)
 *   Volume Down  : SoC Keypad controller at 0x10010000, key 1 (KP_MEM1 bit 1)
 *   Power / Lock : MT6359 PMIC via PMIC Wrapper (PWRAP) at 0x10026000:
 *                  MT6359_TOPSTATUS (0x002a), PWRKEY_DEB (bit 1, active-low)
 *
 * Activated when boot-arg buttons=2 (or buttons=1) is present.
 * Set boot-arg keylog=1 for real-time keypress diagnostic logging.
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define MT6833_KPD_PHYS         0x10010000ULL
#define MT6833_KPD_SIZE         0x1000
#define KP_MEM1                 0x0004          /* keys 0..15, bit clear while key is pressed */

#define MT6833_PWRAP_PHYS       0x10026000ULL
#define MT6833_PWRAP_SIZE       0x1000
#define PWRAP_WACS2_CMD         0x0c80
#define PWRAP_WACS2_VLDCLR      0x0ca4
#define PWRAP_WACS2_RDATA       0x0ca8
#define PWRAP_FSM(r)            (((r) >> 16) & 7u)
#define PWRAP_FSM_IDLE          0
#define PWRAP_FSM_WFVLDCLR      6

#define MT6359_TOPSTATUS        0x002a          /* PMIC top status register */
#define PWRKEY_DEB              (1u << 1)       /* clear while power key is down */

static vm_offset_t kpd_base;
static vm_offset_t pwrap_base;

static inline uint32_t
kpd_rd(uint32_t off)
{
	return *(volatile uint32_t *)(kpd_base + off);
}

static inline uint32_t
pwrap_rd(uint32_t off)
{
	return *(volatile uint32_t *)(pwrap_base + off);
}

static inline void
pwrap_wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(pwrap_base + off) = val;
}

static bool
pwrap_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 100000; spin++) {
		if (PWRAP_FSM(pwrap_rd(PWRAP_WACS2_RDATA)) == state) {
			return true;
		}
	}
	return false;
}

static bool
pmic_read(uint32_t reg, uint32_t *value)
{
	if (!pwrap_wait(PWRAP_FSM_IDLE)) {
		return false;
	}
	pwrap_wr(PWRAP_WACS2_CMD, (reg >> 1) << 16);
	if (!pwrap_wait(PWRAP_FSM_WFVLDCLR)) {
		return false;
	}
	*value = pwrap_rd(PWRAP_WACS2_RDATA) & 0xffff;
	pwrap_wr(PWRAP_WACS2_VLDCLR, 1);
	return true;
}

struct mt6833_key {
	const char *name;
	uint32_t button;        /* HAKIOS_BUTTON_* */
	bool down, seen;        /* debounced state and last sampled state */
};

static struct mt6833_key keys[3] = {
	{ "volume up",   HAKIOS_BUTTON_VOLUME_UP,   false, false },
	{ "volume down", HAKIOS_BUTTON_VOLUME_DOWN, false, false },
	{ "power",       HAKIOS_BUTTON_LOCK,        false, false },
};

static void
mt6833_keys_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t log = 0, presses = 0;

	PE_parse_boot_argn("keylog", &log, sizeof(log));
	kpd_base = ml_io_map(MT6833_KPD_PHYS, MT6833_KPD_SIZE);
	pwrap_base = ml_io_map(MT6833_PWRAP_PHYS, MT6833_PWRAP_SIZE);

	if (!kpd_base || !pwrap_base) {
		printf("mt6833_keys: failed to map IO registers (kpd=%p, pwrap=%p)\n",
		       (void *)kpd_base, (void *)pwrap_base);
		return;
	}

	printf("mt6833_keys: MT6833 keypad @ %#llx, PWRAP @ %#llx; PMIC status %s\n",
	       MT6833_KPD_PHYS, MT6833_PWRAP_PHYS,
	       pwrap_wait(PWRAP_FSM_IDLE) ? "ready" : "TIMEOUT");

	for (unsigned tick = 1;; tick++) {
		uint32_t mem = kpd_rd(KP_MEM1);
		uint32_t top = PWRKEY_DEB;
		bool now[3];

		(void)pmic_read(MT6359_TOPSTATUS, &top);

		/* Keys are active-low (bit cleared when pressed) */
		now[0] = !(mem & (1u << 0));        /* Volume Up */
		now[1] = !(mem & (1u << 1));        /* Volume Down */
		now[2] = !(top & PWRKEY_DEB);       /* Power Button */

		for (unsigned i = 0; i < 3; i++) {
			struct mt6833_key *k = &keys[i];

			/* 2-sample debounce filter */
			if (now[i] == k->seen && now[i] != k->down) {
				k->down = now[i];
				hakios_touch_post(k->down ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP,
				                  k->button, 0, 0);
				presses += k->down;
				if (log) {
					printf("mt6833_keys: %s %s (keypad=%#x, pmic=%#x)\n",
					       k->name, k->down ? "DOWN" : "UP", mem, top);
				}
			}
			k->seen = now[i];
		}

		if (tick % 3000 == 0) {
			printf("mt6833_keys: %u presses recorded; keypad=%#x, pmic=%#x\n",
			       presses, mem, top);
		}

		delay(20000);   /* Poll every 20 ms */
	}
}

void
hakios_mtk_keys_init(void)
{
	uint32_t which = 0;
	thread_t thread;

	if (PE_parse_boot_argn("buttons", &which, sizeof(which)) && which == 0) {
		printf("mt6833_keys: disabled by boot-arg buttons=0\n");
		return;
	}

	printf("mt6833_keys: starting button monitoring thread\n");
	if (kernel_thread_start(mt6833_keys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("mt6833_keys: failed to start button monitoring thread\n");
	}
}
