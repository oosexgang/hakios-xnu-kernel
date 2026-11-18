/*
 * The PMIC's real-time clock, read once at boot: a 32-bit count of seconds that a Qualcomm PMIC keeps
 * running while the phone is off, on the coin cell or the battery. It is not the time of day. The
 * operating system cannot set it on these phones (the firmware keeps it to itself), so Android keeps
 * the difference between it and the right time in a file of its own and adds the two at every boot;
 * Darwin's process 1 does the same here (init/init.c, clock_rtc(): /private/var/db/rtc-offset, written
 * when the host gave the time with epoch=). This driver only gives the count:
 *
 *   hw.rtc_seconds     the count now: what was read at boot, plus the whole seconds since (read only;
 *                      ENOENT when nothing was read, as a kernel without the driver answers)
 *
 * Where it is: the board file's rtc { } names the PMIC's RTC_RW block (slave << 8 | the high byte of its
 * registers): rtc@6100 of the PMK8350 on the SM8350, the SM7325 and after, rtc@6000 of the PM8150, the
 * PM8250 and the PM8998 before them. A board without the section has no clock read. The arbiter is the
 * SoC file's spmi { }.
 *
 * THIS DRIVER ONLY READS. Nothing is written to the PMIC: not the counter (its write registers are the
 * firmware's, and a wrong value there is the wrong time for Android as well), not its control register.
 * The only thing written is the command word that asks the arbiter for a read.
 *
 *   rtc=0      left off: nothing read, no hw.rtc_seconds. On by default, on a board that names the clock.
 *
 * Within the block, from Linux's drivers/rtc/rtc-pm8xxx.c (pm8941_regs and pmk8350_regs: the same layout,
 * the PMK8350's a block further on) and the vendor's qpnp-rtc.c:
 *   0x05       the subtype: 0x01 for the RTC_RW block (qpnp-rtc.c's RTC_PERPH_SUBTYPE; its alarm is 0x03)
 *   0x46       control: bit 7 set while the counter runs
 *   0x48       the count, four bytes, least significant first (RDATA0 .. RDATA3)
 * The four bytes are not latched together: a carry out of the low byte between its read and the next
 * ones gives a count up to 255 s off. As Linux does, the low byte is read once more after the four, and
 * if it went down, a carry came through and the four are read again.
 *
 * The read is made at once, in the start function, not in a thread: it is a few transfers of some tens
 * of microseconds, and process 1 asks for the count as soon as it runs, which a thread might not have
 * reached by then.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/board/input.h>        /* hakios_spmi_lock(): one transfer at a time with the other drivers of PMICs */
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "kernel.h"
#include "board.h"
#include "log.h"

HAKIOS_DEFINE_SAY(true)

#include "mmio.h"
#include "spmi.h"

#define RTC_REG_TYPE            0x04
#define RTC_REG_SUBTYPE         0x05
#define RTC_SUBTYPE_RW          0x01
#define RTC_REG_CTRL            0x46
#define RTC_CTRL_ENABLE         0x80
#define RTC_REG_READ            0x48

static struct hakios_spmi arb;
static uint32_t channel;
static const char *name = "the PMIC's RTC";

/* what was read, and when: hw.rtc_seconds counts on from these */
static bool have_count;
static uint32_t count_at_boot;
static uint64_t read_at;                /* mach_absolute_time() of the read */

static const char *
rtc_read(uint8_t reg, uint8_t *bytes, uint32_t n)
{
	const char *problem;

	hakios_spmi_lock();
	problem = hakios_spmi_read(hakios_spmi_observer(&arb, channel), reg, bytes, n);
	hakios_spmi_unlock();
	return problem;
}

/* The channel of the clock's block: the first, unless a later one is this system's (as buttons.c chooses). */
static bool
find_channel(uint16_t id)
{
	bool found = false;

	for (uint32_t n = 0; n < arb.channels; n++) {
		if (SPMI_ID_OF(hakios_spmi_channel_map(&arb, n)) != id) {
			continue;
		}
		if (!found || hakios_spmi_channel_owner(&arb, n) == arb.system) {
			found = true;
			channel = n;
		}
	}
	return found;
}

/* The count, its carry caught: NULL, or what went wrong. */
static const char *
read_count(uint32_t *seconds, bool *carried)
{
	uint8_t b[4], again;
	const char *problem = rtc_read(RTC_REG_READ, b, 4);

	if (problem == NULL) {
		problem = rtc_read(RTC_REG_READ, &again, 1);
	}
	*carried = problem == NULL && again < b[0];
	if (*carried) {
		problem = rtc_read(RTC_REG_READ, b, 4);
	}
	if (problem == NULL) {
		*seconds = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
	}
	return problem;
}

static int
sysctl_rtc_seconds SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	uint64_t since, value;

	if (!have_count) {
		return ENOENT;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - read_at, &since);
	value = count_at_boot + since / NSEC_PER_SEC;
	return SYSCTL_OUT(req, &value, sizeof(value));
}

SYSCTL_PROC(_hw, OID_AUTO, rtc_seconds, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_rtc_seconds, "Q",
    "the PMIC's real-time clock: seconds it has counted (not the time of day)");

void
hakios_rtc_init(void)
{
	const struct hakios_board *board = hakios_board();
	uint32_t enabled = 1, seconds = 0;
	uint8_t kind[2], control;
	const char *problem;
	clock_sec_t calendar;
	clock_usec_t micro;
	uint16_t id;
	bool carried;

	/* boot-arg rtc=0: the PMIC's clock left alone, nothing read and hw.rtc_seconds not answered */
	PE_parse_boot_argn("rtc", &enabled, sizeof(enabled));
	if (!enabled) {
		return;
	}
	if (board == NULL || board->rtc.peripheral == 0) {
		say("rtc: this board's board file names no clock of a PMIC (rtc { }): none read\n");
		return;
	}
	id = board->rtc.peripheral;
	if (board->rtc.name != NULL) {
		name = board->rtc.name;
	}
	if (!hakios_spmi_map(&arb)) {
		say("rtc: no PMIC arbiter this driver knows (spmi { } of the SoC file; version %#x): %s not read\n", arb.version, name);
		return;
	}
	if (!find_channel(id)) {
		say("rtc: no channel of the arbiter is %s (slave %u, registers %#x): not read\n", name, id >> 8, (id & 0xff) << 8);
		return;
	}
	problem = rtc_read(RTC_REG_TYPE, kind, 2);
	if (problem == NULL) {
		problem = rtc_read(RTC_REG_CTRL, &control, 1);
	}
	if (problem != NULL) {
		say("rtc: %s (slave %u, registers %#x): %s\n", name, id >> 8, (id & 0xff) << 8, problem);
		return;
	}
	/* (a board file that names another block would give a count of something else, taken for seconds) */
	if (kind[1] != RTC_SUBTYPE_RW) {
		say("rtc: %s (slave %u, registers %#x) is of type %#x, subtype %#x: not the RTC's block (subtype %#x), not read\n",
		    name, id >> 8, (id & 0xff) << 8, kind[0], kind[1], RTC_SUBTYPE_RW);
		return;
	}
	if (!(control & RTC_CTRL_ENABLE)) {
		/* starting it would be a write; a count that does not run says nothing of how long the phone was off */
		say("rtc: %s is stopped (control %#x): not read\n", name, control);
		return;
	}
	problem = read_count(&seconds, &carried);
	if (problem != NULL) {
		say("rtc: reading %s: %s\n", name, problem);
		return;
	}
	read_at = mach_absolute_time();
	count_at_boot = seconds;
	have_count = true;
	clock_get_calendar_microtime(&calendar, &micro);
	say("rtc: %s (slave %u, registers %#x, channel %u, type %#x) reads %u s%s; the calendar reads %lu, %ld s more: "
	    "hw.rtc_seconds\n", name, id >> 8, (id & 0xff) << 8, channel, kind[0], seconds,
	    carried ? " (read again: the low byte carried)" : "", (unsigned long)calendar, (long)calendar - (long)seconds);
}
