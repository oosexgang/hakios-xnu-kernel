/*
 * When this kernel was built, as the earliest the calendar can be: a phone with no clock the kernel can read would
 * otherwise start in 1970, and the first answer from a time server then moves it some fifty-six years at once.
 * Darwin's daemons do not take that well: dasd walks its budgets forward in fifteen-minute steps from the last time
 * it stored, two million of them, each a query to coreduetd, and one of the big cores is busy with it for the rest
 * of the boot. Started at the kernel's build instead, the clock moves days or months when the network answers, and
 * nobody walks far. After the OnePlus Nord 2 port of iOS (frxncyyy/ios-denniz, the idea of its piece 0530), which
 * found its phone warm in standby for this.
 *
 * The date is read from the kernel's own version string, "Darwin Kernel Version 26.0.0: Wed Nov 18 10:00:00 UTC
 * 2026; root:...", where Apple's build (config/newvers.pl) writes what `date` said; kernel/build.sh has it said in
 * English and in UTC (KERNEL_BUILD_DATE). The kernel takes it as its floor (iokit/Kernel/GenericARMPE.cpp), and
 * process 1 as the point no stamp of a volume may be more than ten years past (init/init.c), so the reading is kept
 * here, once, in plain C for both and for the host's test (workbench/init_test.c).
 */
#ifndef BUILD_DATE_H
#define BUILD_DATE_H

/*
 * 18 November 2026, when this was written: no right clock reads earlier, so it is the floor even when the version
 * string does not read as a date (a build whose `date` spoke another language).
 */
#define BUILD_DATE_EARLIEST     1794960000L

/* the digits at *P, at most MAX of them, as a number (-1 for none); *P is moved past them */
static inline long
build_date_number(const char **p, int max)
{
	long n = -1;

	for (int i = 0; i < max && **p >= '0' && **p <= '9'; i++, (*p)++) {
		n = (n < 0 ? 0 : n * 10) + (**p - '0');
	}
	return n;
}

static inline void
build_date_spaces(const char **p)
{
	while (**p == ' ') {
		(*p)++;
	}
}

/*
 * The seconds since 1970 of the build date in VERSION, or 0 when it does not read as one. A zone other than UTC or GMT
 * is taken a day early: none is a whole day from Greenwich, so the answer is never later than the build was.
 */
static inline long
build_date_seconds(const char *version)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	const char *p = version;
	long year, month = 0, day, hour, minute, second, days;
	int utc;

	if (p == NULL) {
		return 0;
	}
	while (*p != 0 && !(p[0] == ':' && p[1] == ' ')) {
		p++;
	}
	if (*p == 0) {
		return 0;
	}
	p += 2;
	build_date_spaces(&p);
	/* the day of the week, three letters, then the month's name */
	for (int i = 0; i < 3; i++, p++) {
		if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) {
			return 0;
		}
	}
	build_date_spaces(&p);
	for (int m = 0; m < 12 && month == 0; m++) {
		if (p[0] == months[3 * m] && p[1] == months[3 * m + 1] && p[2] == months[3 * m + 2]) {
			month = m + 1;
		}
	}
	if (month == 0) {
		return 0;
	}
	p += 3;
	build_date_spaces(&p);
	day = build_date_number(&p, 2);
	build_date_spaces(&p);
	hour = build_date_number(&p, 2);
	if (*p++ != ':') {
		return 0;
	}
	minute = build_date_number(&p, 2);
	if (*p++ != ':') {
		return 0;
	}
	second = build_date_number(&p, 2);
	if (day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60 ||
	    *p != ' ') {
		return 0;
	}
	build_date_spaces(&p);
	utc = (p[0] == 'U' && p[1] == 'T' && p[2] == 'C' && p[3] == ' ') ||
	    (p[0] == 'G' && p[1] == 'M' && p[2] == 'T' && p[3] == ' ');
	while (*p != 0 && *p != ' ' && *p != ';') {
		p++;            /* the zone, whatever it is called */
	}
	build_date_spaces(&p);
	year = build_date_number(&p, 4);
	if (year < 2000 || year > 2099 || (*p != 0 && *p != ';' && *p != ' ')) {
		return 0;
	}
	/* days since 1 January 1970 of the civil date (the year counted from March, so that February comes last) */
	if (month <= 2) {
		year--;
	}
	days = year * 365 + year / 4 - year / 100 + year / 400 + (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 +
	    day - 1 - 719468;
	return days * 86400 + hour * 3600 + minute * 60 + second - (utc ? 0 : 86400);
}

/* The calendar's floor: the build date, or BUILD_DATE_EARLIEST when that is later or there is none. */
static inline long
build_date_floor(const char *version)
{
	long built = build_date_seconds(version);

	return built > BUILD_DATE_EARLIEST ? built : BUILD_DATE_EARLIEST;
}

#endif /* BUILD_DATE_H */
