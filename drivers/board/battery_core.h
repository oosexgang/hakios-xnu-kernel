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

#endif /* HAKIOS_BATTERY_CORE_H */
