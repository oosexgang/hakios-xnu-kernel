/*
 * The Infinix Hot 20's battery as iOS reads it. Boot-arg mtkbatt=1; mtkbatt=2 says every reading in the log, every 2 s
 * instead of every 10; mtkbatt=3 takes the percentage from the voltage alone (the way without Android's number).
 *
 * The percentage is Android's own, carried on by the PMIC's counter of charge:
 *  - Android's gauge is a program, and it leaves the percentage it shows in the PMIC's clock, bits 14:8 of RTC_AL_MTH (0x5aa):
 *    0xe401 while Android said 100, 0xe301 while it said 99 (10 November 2026, read from Android through pmic_access).
 *  - The MT6358's fuel gauge counts the charge through the phone's 5 mOhm (R_FG_VALUE of its device tree) in FGADC_CAR_CON0/1
 *    (0xd14, 0xd16), and Android does not start the counter again: it went on from XNU's last number after run 764. Read after
 *    a latch (FGADC_CON1, 0xd0a: bit 0 asks, bit 15 says latched, bit 3 lets go) with the current, FGADC_CUR_CON0 (0xd8a):
 *    16 bits with a sign, 250 mV / 65536 a count over 5 mOhm = 0.763 mA. Run 764 saw -240..-437 mA on a computer's port
 *    (Android says about the same there, -357 mA), the voltage lower the more negative it was, and the counter falling:
 *    less than 0 is out of the battery. The counter fell 92546 in the 90.8 s that the current summed to 9.94 mAh:
 *    0.1074 uAh a count. The cell is 5132 mAh (the 25 degree table of the phone's profile), 51 mAh a point.
 *  - So: the percentage in the clock when the kernel starts, plus the counter's change since. Not written back to the clock:
 *    Android's gauge makes its own on its next start.
 * Checked against the voltage, and without Android's number (0, or over 100) it is the voltage alone:
 *  - The MT6358's BATADC, channel 0 of its AUXADC. A conversion is asked for (bit 0 of AUXADC_RQST0, 0x1108) and its result
 *    is AUXADC_ADC0 (0x1088: 15 bits, bit 15 "ready"). Without the request the register keeps the last conversion somebody
 *    made: run 754 read 26788 counts at every look for a minute, which was Android's last. A count is 1.8 V x 3 / 32768 =
 *    0.16479 mV: 26775 counts while Android's own gauge said 4414 mV.
 *  - The cell at rest by a curve of a 4.45 V Li-ion cell, checked at ten points against the phone's profile: 4345 mV is 90%,
 *    3885 mV 50%, 3690 mV 10%. Under a load the voltage is brought back by the current over 0.28 Ohm, which is what run
 *    764's own pairs give (4306 mV at -240 mA, 4254 mV at -422 mA) and more than the profile's 0.13: it is rough, 4276 mV
 *    at -357 mA is 94% by it where Android says 99. If the two ways are more than 30 points apart the voltage is believed.
 * A charger on the port: RGS_CHRDET, bit 5 of CHR_CON0 (0xa88), the name Android's PMIC header gives it; 0x62 with the cable
 * in. It has not been seen at 0 yet (the cable is how the phone is run). "Charging" is said only while the current goes in.
 * Not done: the charger itself (RT9471 on I2C5: nothing sets or feeds it here), the temperature (25 degrees is said).
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

#define MT6358_RTC_AL_MTH       0x05aa          /* bits 14:8: the percentage Android's gauge shows */
#define MT6358_CHR_CON0         0x0a88
#define CHR_CON0_CHRDET         (1u << 5)
#define MT6358_FGADC_CON0       0x0d08
#define MT6358_FGADC_CON1       0x0d0a
#define FGADC_CON1_READ         (1u << 0)
#define FGADC_CON1_CLEAR        (1u << 3)
#define FGADC_CON1_LATCHED      (1u << 15)
#define MT6358_FGADC_CAR_CON0   0x0d14
#define MT6358_FGADC_CAR_CON1   0x0d16
#define MT6358_FGADC_CUR_CON0   0x0d8a
#define MT6358_AUXADC_ADC0      0x1088          /* BATADC */
#define MT6358_AUXADC_RQST0     0x1108

#define COUNT_NV                164795          /* 1.8 V x 3 / 32768, in nV a count */
#define FG_COUNT_NA             762939          /* 250 mV / 65536 over 5 mOhm, in nA a count */
#define CAR_COUNT_PAH           107425          /* run 764: 9.94 mAh by the current while the counter fell 92546, in pAh a count */
#define CELL_UAH                5132000         /* the phone's profile at 25 degrees */
#define CELL_MOHM               280
#define FULL_MV                 4410
#define APART_HUNDREDTHS        3000

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

static bool
pmic_write(uint32_t reg, uint16_t value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (pwrap_wait(0)) {
		pwrap[PWRAP_WACS2_CMD / 4] = (1u << 31) | ((reg >> 1) << 16) | value;
		ok = pwrap_wait(0);
	}
	redwood_pwrap_unlock();
	return ok;
}

/* the register's bits (mask) at `want`, looked at every millisecond for `ms` */
static bool
pmic_until(uint32_t reg, uint16_t mask, uint16_t want, unsigned ms, uint16_t *last)
{
	for (unsigned t = 0; t <= ms; t++) {
		if (pmic_read(reg, last) && (*last & mask) == want) {
			return true;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	return false;
}

/* one conversion of the battery's voltage, asked for now: counts, or -1 with the reason said once */
static int32_t
batadc_convert(void)
{
	static bool said;
	uint16_t raw = 0;

	if (pmic_write(MT6358_AUXADC_RQST0, 1)) {
		/* the last conversion's "ready" may still be up: the converter is given its time before the look */
		delay_for_interval(3, NSEC_PER_MSEC);
		if (pmic_until(MT6358_AUXADC_ADC0, 0x8000, 0x8000, 50, &raw)) {
			return raw & 0x7fff;
		}
	}
	if (!said) {
		said = true;
		printf("mtkbatt: no conversion of BATADC in 50 ms after the request (ADC0 %#x): no reading is given\n", raw);
	}
	return -1;
}

/*
 * The fuel gauge's current and its counter of charge, latched together. False, with the reason said once, if the gauge does
 * not latch (it is Android that switched it on; nothing here does).
 */
static bool
gauge_latch(int16_t *current, uint32_t *charge)
{
	static bool said;
	uint16_t con1 = 0, got = 0, low = 0, high = 0, cur = 0;
	bool ok;

	if (!pmic_read(MT6358_FGADC_CON1, &con1)) {
		return false;
	}
	con1 &= (uint16_t)~(FGADC_CON1_READ | FGADC_CON1_CLEAR | FGADC_CON1_LATCHED);
	ok = pmic_write(MT6358_FGADC_CON1, con1 | FGADC_CON1_READ) &&
	    pmic_until(MT6358_FGADC_CON1, FGADC_CON1_LATCHED, FGADC_CON1_LATCHED, 100, &got) &&
	    pmic_read(MT6358_FGADC_CUR_CON0, &cur) && pmic_read(MT6358_FGADC_CAR_CON0, &low) && pmic_read(MT6358_FGADC_CAR_CON1, &high);
	/* let go, whatever happened */
	(void)pmic_write(MT6358_FGADC_CON1, con1 | FGADC_CON1_CLEAR);
	(void)pmic_until(MT6358_FGADC_CON1, FGADC_CON1_LATCHED, 0, 100, &got);
	(void)pmic_write(MT6358_FGADC_CON1, con1);
	if (!ok) {
		if (!said) {
			uint16_t con0 = 0;

			said = true;
			(void)pmic_read(MT6358_FGADC_CON0, &con0);
			printf("mtkbatt: the fuel gauge does not latch (FGADC_CON0 %#x, CON1 %#x): no current\n", con0, got);
		}
		return false;
	}
	*current = (int16_t)cur;
	*charge = ((uint32_t)high << 16) | low;
	return true;
}

/* the cell's charge at rest by its voltage, in hundredths of a percent: (mV, hundredths) */
static int32_t
percent_of(int32_t mv)
{
	static const struct { int32_t mv, hundredths; } curve[] = {
		{ 3400, 0 }, { 3620, 500 }, { 3690, 1000 }, { 3745, 2000 }, { 3785, 3000 }, { 3825, 4000 }, { 3885, 5000 },
		{ 3985, 6000 }, { 4100, 7000 }, { 4225, 8000 }, { 4345, 9000 }, { 4395, 9700 }, { FULL_MV, 10000 },
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
	int32_t kept_mv = 0;            /* the cell's voltage at rest, smoothed over four looks */
	int32_t kept_ma = 0;            /* the current, the same way: "charging" does not flicker with it */
	int32_t start_hundredths = -1;  /* Android's percentage, or -1 */
	uint32_t start_charge = 0;
	uint16_t rtc = 0;
	bool counted = false, apart_said = false;

	PE_parse_boot_argn("mtkbatt", &level, sizeof(level));
	pwrap = (volatile uint32_t *)ml_io_map(PWRAP_BASE, 0x1000);
	if (pmic_read(MT6358_RTC_AL_MTH, &rtc) && ((rtc >> 8) & 0x7f) >= 1 && ((rtc >> 8) & 0x7f) <= 100) {
		start_hundredths = (int32_t)((rtc >> 8) & 0x7f) * 100;
	}
	delay_for_interval(20000, NSEC_PER_MSEC);
	for (;; delay_for_interval(level == 2 ? 2000 : 10000, NSEC_PER_MSEC)) {
		struct redwood_battery_state b = { .size = sizeof(b) };
		int32_t sum = 0, good = 0, low = 0x7fff, high = 0, mv, rest_mv, ma = 0, by_voltage, by_charge = -1;
		uint16_t chr = 0;
		int16_t gauge = 0;
		uint32_t charge = 0;
		bool charger, current;

		for (unsigned i = 0; i < 4; i++) {
			int32_t counts = batadc_convert();

			if (counts >= 0) {
				sum += counts;
				good++;
				low = counts < low ? counts : low;
				high = counts > high ? counts : high;
			}
		}
		if (good == 0) {
			continue;       /* nothing is said to iOS on a look that read nothing: it keeps what it had */
		}
		mv = (int32_t)((int64_t)(sum / good) * COUNT_NV / 1000000);
		if (mv < 2500 || mv > 4700) {
			printf("mtkbatt: BATADC %d counts would be %d mV: not a cell's voltage, not given\n", sum / good, mv);
			continue;
		}
		charger = pmic_read(MT6358_CHR_CON0, &chr) && (chr & CHR_CON0_CHRDET);
		current = gauge_latch(&gauge, &charge);
		if (current) {
			ma = (int32_t)((int64_t)gauge * FG_COUNT_NA / 1000000);
		}
		rest_mv = mv - ma * CELL_MOHM / 1000;
		kept_mv = kept_mv == 0 ? rest_mv : (kept_mv * 3 + rest_mv) / 4;
		kept_ma = (kept_ma * 3 + ma) / 4;
		by_voltage = percent_of(kept_mv);
		if (current && start_hundredths >= 0 && level != 3) {
			if (!counted) {
				counted = true;
				start_charge = charge;
				printf("mtkbatt: Android left %d%% (RTC_AL_MTH %#x); the charge is counted from %#x\n", start_hundredths / 100, rtc, charge);
			}
			/* hundredths of a percent: counts x pAh / (uAh x 100) */
			by_charge = start_hundredths + (int32_t)((int64_t)(int32_t)(charge - start_charge) * CAR_COUNT_PAH / ((int64_t)CELL_UAH * 100));
			by_charge = by_charge < 0 ? 0 : by_charge > 10000 ? 10000 : by_charge;
			if (by_charge - by_voltage > APART_HUNDREDTHS || by_voltage - by_charge > APART_HUNDREDTHS) {
				if (!apart_said) {
					apart_said = true;
					printf("mtkbatt: %d%% by the charge and %d%% by the voltage (%d mV at rest): the voltage is believed from here\n",
					    by_charge / 100, by_voltage / 100, kept_mv);
				}
				start_hundredths = -1;
				by_charge = -1;
			}
		} else if (!counted && !apart_said && looks == 0) {
			printf("mtkbatt: the percentage is by the voltage alone (RTC_AL_MTH %#x, %s, mtkbatt=%u)\n", rtc,
			    current ? "the gauge answers" : "no gauge", level);
		}
		b.percent_hundredths = by_charge >= 0 ? by_charge : by_voltage;
		b.voltage_uv = mv * 1000;
		b.current_ua = current ? ma * 1000 : 0;
		b.temperature_hundredths = 2500;        /* not read: 25 degrees */
		b.present = 1;
		b.external = charger;
		/* Linux's numbers: 1 charging, 2 discharging, 3 not charging, 4 full */
		if (!charger) {
			b.status = 2;
		} else if (b.percent_hundredths >= 9950) {
			b.status = 4;
		} else if (current && kept_ma <= 0) {
			b.status = 3;
		} else {
			b.status = 1;
		}
		redwood_battery_report(&b);
		if (level >= 2 || (looks % 30) == 0) {
			printf("mtkbatt: %d.%02d%% (%s), BATADC %d counts (%d..%d of %d) = %d mV, charger %s (CHR_CON0 %#x), status %d",
			    b.percent_hundredths / 100, b.percent_hundredths % 100, by_charge >= 0 ? "by the charge" : "by the voltage",
			    sum / good, low, high, good, mv, charger ? "in" : "out", chr, b.status);
			if (current) {
				printf("; gauge %d counts = %d mA, charge counter %#x, at rest %d mV = %d.%02d%% by the voltage\n", gauge, ma, charge,
				    kept_mv, by_voltage / 100, by_voltage % 100);
			} else {
				printf("; no current\n");
			}
		}
		looks++;
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
