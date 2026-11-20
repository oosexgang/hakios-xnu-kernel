/*
 * What the battery's charge is shown as, from what a Qualcomm UEFI's charger drivers say (bsd/dev/board/battery.c):
 * the arithmetic alone, so that the host tests it (workbench/battery_driver_test.c).
 *
 * The firmware's own state of charge cannot always be believed. On redwood it said 15% in every run of a day while
 * the cell stood at 4.27-4.41 V -- a figure from when the firmware started, never updated -- and its voltage is the
 * cell's under whatever current flows: lower while the phone draws more than the port gives, higher while it
 * charges. So the charge is also judged from the voltage: the open-circuit voltage is what the cell reads plus what
 * the current costs in its resistance, and a lithium-ion cell's open-circuit voltage gives its charge (the table).
 * The firmware's figure is taken when the two agree; the voltage's when they do not. What is shown moves toward that
 * a little at every look, never down while the battery takes charge and never up while it gives it, so that a
 * moment's load does not make the percentage jump about.
 *
 * Currents here are the firmware's way round: more than 0 while the battery gives charge (redwood's logs: +200 to
 * +557 mA with the cell sagging to 4.27 V while the system started, -340 to -400 mA at 4.39-4.41 V once it rested on
 * a computer's port).
 */
#ifndef HAKIOS_BATTERY_CORE_H
#define HAKIOS_BATTERY_CORE_H

#define BAT_R_MOHM              80      /* the cell's and its path's resistance: redwood's readings above, 4347 mV at +557 mA and 4410 at -397 */
#define BAT_AGREE_TENTHS        150     /* the firmware's figure is believed within 15 points of the voltage's */
#define BAT_GAINING_MA          (-20)   /* below this (into the battery) it takes charge */
#define BAT_STEP_TENTHS         2       /* how far what is shown moves at a look (every 5 s: 2.4% a minute) ... */
#define BAT_FAR_TENTHS          100     /* ... unless it is this far off ... */
#define BAT_FAR_STEP_TENTHS     20      /* ... when it moves this far */
/*
 * A current no phone's cell carries, either way: what the firmware says beyond it is not a current (its word of the
 * charger's status is unchecked all the way from the loader) and is taken for none. Believed, it moved the voltage's
 * estimate by its 80 mOhm -- 60 A said to go into the cell took 4.8 V off, a battery at 70% read as empty and shown
 * as 0% at the first look, for iOS to shut the phone down on (60 A out of it: full) -- and was multiplied past what
 * an int holds.
 */
#define BAT_MOST_MA             20000

/* a cell of 4.4 to 4.45 V (Xiaomi's), its open-circuit voltage against its charge in tenths of a percent */
static const struct {
	int mv, tenths;
} bat_ocv_table[] = {
	{ 3400, 0 }, { 3500, 20 }, { 3600, 50 }, { 3680, 100 }, { 3730, 150 }, { 3760, 200 }, { 3790, 300 },
	{ 3830, 400 }, { 3880, 500 }, { 3940, 600 }, { 4010, 700 }, { 4090, 800 }, { 4180, 900 }, { 4280, 950 },
	{ 4380, 1000 },
};

/* the open-circuit voltage of a cell that reads MV with DISCHARGE_MA flowing out of it */
static inline int
bat_ocv_mv(int mv, int discharge_ma)
{
	return mv + discharge_ma * BAT_R_MOHM / 1000;
}

/* the charge at an open-circuit voltage, tenths of a percent */
static inline int
bat_tenths_at(int ocv_mv)
{
	const unsigned n = sizeof(bat_ocv_table) / sizeof(bat_ocv_table[0]);

	if (ocv_mv <= bat_ocv_table[0].mv) {
		return 0;
	}
	for (unsigned i = 1; i < n; i++) {
		if (ocv_mv < bat_ocv_table[i].mv) {
			const int dm = bat_ocv_table[i].mv - bat_ocv_table[i - 1].mv, dt = bat_ocv_table[i].tenths - bat_ocv_table[i - 1].tenths;

			return bat_ocv_table[i - 1].tenths + (ocv_mv - bat_ocv_table[i - 1].mv) * dt / dm;
		}
	}
	return 1000;
}

/*
 * The charge, tenths of a percent: the firmware's GAUGE_PERCENT (taken as an estimate of its own when it is one)
 * when it agrees with the voltage's, else the voltage's. MV < 0: no voltage, and the firmware's figure as it is.
 */
static inline int
bat_estimate_tenths(int gauge_percent, int gauge_estimated, int mv, int discharge_ma)
{
	int from_voltage;

	if (mv < 0) {
		return gauge_percent * 10;
	}
	from_voltage = bat_tenths_at(bat_ocv_mv(mv, discharge_ma));
	if (gauge_estimated || gauge_percent < 0) {
		return from_voltage;
	}
	return gauge_percent * 10 - from_voltage <= BAT_AGREE_TENTHS && from_voltage - gauge_percent * 10 <= BAT_AGREE_TENTHS ?
	       gauge_percent * 10 : from_voltage;
}

/*
 * What is shown after a look: SHOWN (< 0: nothing yet) toward ESTIMATE -- up only while GAINING, down only while not,
 * a little at a time; but either way, faster, when it is more than BAT_FAR_TENTHS off (a first look under a load the
 * resistance did not account for is not to be shown for an hour)
 */
static inline int
bat_shown_tenths(int shown, int estimate, int gaining)
{
	int gap;

	if (shown < 0) {
		return estimate;
	}
	gap = estimate - shown;
	if (gap > BAT_FAR_TENTHS || -gap > BAT_FAR_TENTHS) {
		return shown + (gap > 0 ? BAT_FAR_STEP_TENTHS : -BAT_FAR_STEP_TENTHS);
	}
	if (gap > 0 && gaining) {
		return shown + (gap < BAT_STEP_TENTHS ? gap : BAT_STEP_TENTHS);
	}
	if (gap < 0 && !gaining) {
		return shown - (-gap < BAT_STEP_TENTHS ? -gap : BAT_STEP_TENTHS);
	}
	return shown;
}

/*
 * How long the charge lasts, or how long until the battery is full: what Darwin's power source says beside the
 * percentage (iokit/Kernel/BoardBattery.cpp: TimeRemaining, AvgTimeToEmpty, AvgTimeToFull, minutes), and until now
 * said nothing of. After the idea of the OnePlus Nord 2 port's piece 0701 (frxncyyy/ios-denniz), which says them from
 * its gauge's charge and average current; nothing of its code is here. The fuel gauges met so far give a charge and a
 * current of the moment and no average, so the average is kept here:
 *
 *   the current    averaged over about a minute (BAT_TIME_TAU_MS), each look weighed by the time since the one
 *                  before; begun anew when the battery turns from giving charge to taking it or back (a charger
 *                  put in or pulled out), and when the last look is more than that minute old
 *   nothing said   until BAT_TIME_SETTLE_MS of it have been looked at; while the average is under BAT_TIME_LEAST_UA
 *                  either way (a phone that rests on a port gives and takes by turns); while the battery is on a
 *                  charger and does not take charge; when the charge in the battery (or, to be full, the full
 *                  charge) is not known; and when it comes to more than BAT_TIME_MOST_MINUTES
 *   to empty       the charge in the battery at the average current out of it
 *   to full        what is missing to the full charge at the average current into it -- at this rate: a charger
 *                  lets the current fall as the cell fills, so the last part takes longer than this says and the
 *                  figure is corrected as the average follows the current down
 *
 * Currents here are Darwin's way round (struct hakios_battery_state): more than 0 while the battery takes charge.
 */
#define BAT_TIME_UNKNOWN        (-1)    /* not known (yet): Darwin's own word for it in TimeRemaining */
#define BAT_TIME_OFF            (-2)    /* nothing is to be said of it at all (boot-arg battery_time=0) */
#define BAT_TIME_TAU_MS         60000
#define BAT_TIME_SETTLE_MS      30000
#define BAT_TIME_LEAST_UA       20000   /* (BAT_GAINING_MA: under it the battery neither gives nor takes to speak of) */
#define BAT_TIME_MOST_MINUTES   65534   /* (a gauge's 16 bits, whose 65535 is "not known") */

struct bat_time {
	int started;                    /* a look was taken */
	int way;                        /* 1 taking charge, -1 giving it with no charger, 0 on a charger and not charging */
	int average_ua;                 /* the current, averaged (more than 0 into the battery) */
	unsigned covered_ms;            /* how much time the average is of, to BAT_TIME_SETTLE_MS and no further */
	unsigned long long last_ms;     /* when the last look was */
};

/* which way the charge goes for a battery whose port is EXTERNAL and whose state is STATUS (POWER_SUPPLY_STATUS_*) */
static inline int
bat_time_way(int external, int status)
{
	return !external ? -1 : status == 1 ? 1 : 0;
}

/* A look: CURRENT_UA at NOW_MS (a clock that only grows), the charge going WAY. A current that cannot be is no look. */
static inline void
bat_time_look(struct bat_time *t, int way, int current_ua, unsigned long long now_ms)
{
	unsigned long long dt;

	if (current_ua < -BAT_MOST_MA * 1000 || current_ua > BAT_MOST_MA * 1000) {
		return;
	}
	dt = t->started && now_ms > t->last_ms ? now_ms - t->last_ms : 0;
	if (!t->started || way != t->way || dt > BAT_TIME_TAU_MS) {
		t->started = 1;
		t->way = way;
		t->average_ua = current_ua;
		t->covered_ms = 0;
		t->last_ms = now_ms;
		return;
	}
	t->average_ua += (int)((long long)(current_ua - t->average_ua) * (long long)dt / BAT_TIME_TAU_MS);
	t->covered_ms = t->covered_ms + dt >= BAT_TIME_SETTLE_MS ? BAT_TIME_SETTLE_MS : t->covered_ms + (unsigned)dt;
	t->last_ms += dt;               /* (never back, should the clock be) */
}

/*
 * The charge in the battery, uAh: the gauge's count of it when it has one, else the full charge it has learnt taken
 * at the percentage; 0 when neither is known.
 */
static inline int
bat_time_charge_uah(int charge_uah, int full_uah, int percent_hundredths)
{
	if (charge_uah > 0) {
		return charge_uah;
	}
	if (full_uah <= 0 || percent_hundredths < 0 || percent_hundredths > 10000) {
		return 0;
	}
	return (int)((long long)full_uah * percent_hundredths / 10000);
}

/*
 * The minutes until the battery is empty (while it gives charge) or full (while it takes it), with CHARGE_UAH in it
 * of FULL_UAH (0: not known); BAT_TIME_UNKNOWN when there is nothing to say (above).
 */
static inline int
bat_time_minutes(const struct bat_time *t, int charge_uah, int full_uah)
{
	long long left, rate, minutes;

	if (!t->started || t->way == 0 || t->covered_ms < BAT_TIME_SETTLE_MS || charge_uah <= 0) {
		return BAT_TIME_UNKNOWN;
	}
	if (t->way < 0) {
		left = charge_uah;
		rate = -(long long)t->average_ua;
	} else {
		if (full_uah <= 0) {
			return BAT_TIME_UNKNOWN;
		}
		left = charge_uah < full_uah ? full_uah - charge_uah : 0;
		rate = t->average_ua;
	}
	if (rate < BAT_TIME_LEAST_UA) {
		return BAT_TIME_UNKNOWN;
	}
	minutes = left * 60 / rate;
	return minutes > BAT_TIME_MOST_MINUTES ? BAT_TIME_UNKNOWN : (int)minutes;
}

#endif /* HAKIOS_BATTERY_CORE_H */
