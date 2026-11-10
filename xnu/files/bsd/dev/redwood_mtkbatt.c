/*
 * The Infinix Hot 20's battery as iOS reads it (boot-arg mtkbatt=1; mtkbatt=2 says every reading in the log).
 *
 * What is known: the MT6358 PMIC's BATADC, register 0x1088 over the PMIC wrapper (15 bits, bit 15 "ready"; Android's
 * /sys/devices/platform/1000d000.pwrap/.../pmic_access reads the same value): 26700 counts when the battery is at 4.25 V, so
 * about 0.1595 mV a count (the datasheet's 1.8 V x 3 over 32768 is 3% higher: the converter's gain is not trimmed here).
 * What is not: whether a charger is on the port (the RT9471's status is on I2C5, which XNU has no driver for) and the current,
 * so "external" is 0 and the status "discharging" until they are; the percentage is the voltage's, by a Li-ion curve at rest,
 * which a load or a charger pushes up or down by a few points. This is a gauge made of one register, not Android's.
 * One transaction at a time on the wrapper: redwood_pwrap_lock() is the camera's and CONNSYS's too.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define PWRAP_BASE              0x1000d000ULL
#define PWRAP_WACS2_CMD         0xc20
#define PWRAP_WACS2_RDATA       0xc24
#define PWRAP_WACS2_VLDCLR      0xc28
#define PWRAP_FSM(r)            (((r) >> 16) & 7u)
#define MT6358_AUXADC_ADC0      0x1088          /* BATADC */

static volatile uint32_t *pwrap;

static bool
pwrap_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (PWRAP_FSM(pwrap[PWRAP_WACS2_RDATA / 4]) == state) {
			return true;
		}
	}
	return false;
}

static bool
pmic_read(uint32_t reg, uint16_t *value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (pwrap_wait(0)) {
		pwrap[PWRAP_WACS2_CMD / 4] = (reg >> 1) << 16;
		if (pwrap_wait(6)) {
			*value = (uint16_t)pwrap[PWRAP_WACS2_RDATA / 4];
			pwrap[PWRAP_WACS2_VLDCLR / 4] = 1;
			ok = true;
		}
	}
	redwood_pwrap_unlock();
	return ok;
}

/* a Li-ion cell's charge at rest by its voltage, in hundredths of a percent: (mV, hundredths) */
static int32_t
percent_of(int32_t mv)
{
	static const struct { int32_t mv, hundredths; } curve[] = {
		{ 3300, 0 }, { 3500, 500 }, { 3600, 1000 }, { 3700, 2000 }, { 3750, 3000 }, { 3800, 4000 }, { 3850, 5000 },
		{ 3900, 6000 }, { 3950, 6800 }, { 4000, 7500 }, { 4100, 8800 }, { 4200, 9700 }, { 4250, 10000 },
	};

	if (mv <= curve[0].mv) {
		return 0;
	}
	for (unsigned i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
		if (mv <= curve[i].mv) {
			return curve[i - 1].hundredths + (mv - curve[i - 1].mv) * (curve[i].hundredths - curve[i - 1].hundredths) / (curve[i].mv - curve[i - 1].mv);
		}
	}
	return 10000;
}

static void
mtkbatt_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t level = 1;
	uint32_t looks = 0;

	PE_parse_boot_argn("mtkbatt", &level, sizeof(level));
	pwrap = (volatile uint32_t *)ml_io_map(PWRAP_BASE, 0x1000);
	delay_for_interval(20000, NSEC_PER_MSEC);
	for (;;) {
		uint32_t sum = 0, good = 0;

		for (unsigned i = 0; i < 8; i++) {
			uint16_t raw;

			if (pmic_read(MT6358_AUXADC_ADC0, &raw) && (raw & 0x8000)) {
				sum += raw & 0x7fff;
				good++;
			}
		}
		if (good != 0) {
			int32_t counts = (int32_t)(sum / good), mv = counts * 1595 / 10000;
			struct redwood_battery_state b = { .size = sizeof(b) };

			b.percent_hundredths = percent_of(mv);
			b.voltage_uv = mv * 1000;
			b.temperature_hundredths = 2500;        /* not read: 25 degrees */
			b.present = 1;
			b.status = 2;
			redwood_battery_report(&b);
			if (level >= 2 || (looks++ % 30) == 0) {
				printf("mtkbatt: BATADC %d counts = %d mV, %d.%02d%%\n", counts, mv, b.percent_hundredths / 100, b.percent_hundredths % 100);
			}
		} else if (level >= 2) {
			printf("mtkbatt: BATADC not ready\n");
		}
		delay_for_interval(10000, NSEC_PER_MSEC);
	}
}

void
redwood_mtkbatt_init(void)
{
	uint32_t level = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("mtkbatt", &level, sizeof(level)) || level < 1) {
		return;
	}
	if (kernel_thread_start(mtkbatt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
