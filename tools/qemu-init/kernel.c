/* tools/qemu-init: the kernel's values, messages and time: sysctl, uptime, dmesg, date */
#include "init.h"

/*
 * NAME[=VALUE]...: a value as the kernel types it, and with =VALUE set first: its name to its numbers
 * (sysctl 0.3, name2oid), their kind and format
 * (0.4, oidfmt: CTLTYPE_INT, _STRING, _QUAD, and a format that says whether the number is signed), then
 * the value. What /usr/sbin/sysctl does, without it.
 */
#define SYS___sysctl            202
#define CTLTYPE_MASK            0xf
#define CTLTYPE_NODE            1
#define CTLTYPE_INT             2
#define CTLTYPE_STRING          3
#define CTLTYPE_QUAD            4
#define CTL_MAXNAME             12

static void
say_signed(int64_t n)
{
	if (n < 0) {
		say("-");
		say_number((uint64_t)-n);
	} else {
		say_number((uint64_t)n);
	}
}

void
command_sysctl(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		int query[2] = { 0, 3 }, mib[CTL_MAXNAME + 2] = { 0, 4 };
		size_t size = CTL_MAXNAME * sizeof(int);
		union { uint32_t kind; char bytes[64]; } format;
		union { char text[512]; int32_t i32[128]; int64_t i64[64]; } value;

		/* NAME=VALUE: the value set first (a string, or a number for a number) */
		char *setting = argv[i];
		while (*setting != '\0' && *setting != '=') {
			setting++;
		}
		if (*setting == '=') {
			*setting++ = '\0';
		} else {
			setting = NULL;
		}
		say(argv[i]);
		say(": ");
		if (syscall6(SYS___sysctl, (long)query, 2, (long)&mib[2], (long)&size, (long)argv[i], (long)length(argv[i])) < 0) {
			say("(no such name)\n");
			continue;
		}
		unsigned depth = (unsigned)(size / sizeof(int));
		size = sizeof(format);
		if (syscall6(SYS___sysctl, (long)mib, depth + 2, (long)&format, (long)&size, 0, 0) < 0) {
			say("(no format)\n");
			continue;
		}
		const char *fmt = format.bytes + sizeof(uint32_t);
		int type = (int)(format.kind & CTLTYPE_MASK), is_unsigned = fmt[1] == 'U';
		if (type == CTLTYPE_NODE) {
			say("(a node: its children have the values)\n");
			continue;
		}
		if (setting != NULL) {
			int64_t number = setting[0] == '-' ? -(int64_t)parse_number(setting + 1) : (int64_t)parse_number(setting);
			int32_t number32 = (int32_t)number;
			const void *new_value = type == CTLTYPE_STRING ? (const void *)setting : type == CTLTYPE_INT ? (const void *)&number32 : &number;
			size_t new_size = type == CTLTYPE_STRING ? length(setting) : type == CTLTYPE_INT ? 4 : 8;
			long error = type == CTLTYPE_STRING || type == CTLTYPE_INT || type == CTLTYPE_QUAD ?
			    syscall6(SYS___sysctl, (long)&mib[2], depth, 0, 0, (long)new_value, (long)new_size) : -22;
			if (error < 0) {
				say("(not taken: error ");
				say_number((uint64_t)-error);
				say(")\n");
				continue;
			}
		}
		size = sizeof(value) - 1;
		if (syscall6(SYS___sysctl, (long)&mib[2], depth, (long)&value, (long)&size, 0, 0) < 0) {
			say("(the kernel will not say)\n");
			continue;
		}
		if (type == CTLTYPE_STRING) {
			value.text[size] = '\0';
			say(size > 1 ? value.text : "(empty)");
		} else if (type == CTLTYPE_INT && size >= 4) {
			for (size_t n = 0; n < size / 4; n++) {
				say(n ? " " : "");
				is_unsigned ? say_number((uint32_t)value.i32[n]) : say_signed(value.i32[n]);
			}
		} else if (type == CTLTYPE_QUAD && size >= 8) {
			for (size_t n = 0; n < size / 8; n++) {
				say(n ? " " : "");
				is_unsigned ? say_number((uint64_t)value.i64[n]) : say_signed(value.i64[n]);
			}
		} else {
			say("(");
			say_number(size);
			say(" bytes of ");
			say(fmt);
			say(")");
		}
		say("\n");
	}
}

void
command_uptime(int argc, char **argv)
{
	int64_t booted[2] = { 0, 0 }, now[2] = { 0, 0 };
	size_t size = sizeof(booted);
	(void)argc;
	(void)argv;

	if (syscall6(SYS_sysctlbyname, (long)"kern.boottime", 13, (long)booted, (long)&size, 0, 0) < 0) {
		say("uptime: the kernel will not say when it started\n");
		return;
	}
	if (syscall6(SYS_gettimeofday, (long)now, 0, 0, 0, 0, 0) != 0) {
		say("uptime: the kernel will not say what time it is\n");
		return;
	}
	uint64_t up = (uint64_t)(now[0] - booted[0]);
	say("up ");
	say_number(up / 60);
	say(" min ");
	say_number(up % 60);
	say(" s, since ");
	say_number((uint64_t)booted[0]);
	say(" (seconds since 1970)\n");
}

#define PROC_INFO_CALL_KERNMSGBUF 4

/* [LINES]: the last LINES lines (20 if not said) of the kernel's message buffer */
void
command_dmesg(int argc, char **argv)
{
	static char text[128 * 1024];
	unsigned lines = argc > 1 ? parse_number(argv[1]) : 20;

	long got = syscall6(SYS_proc_info, PROC_INFO_CALL_KERNMSGBUF, 0, 0, 0, (long)text, sizeof(text));
	if (got <= 0 || lines == 0) {
		say(got <= 0 ? "dmesg: the kernel will not give its messages\n" : "dmesg [LINES]: a number of lines\n");
		return;
	}
	while (got > 0 && (text[got - 1] == '\0' || text[got - 1] == '\n')) {
		got--;
	}
	text[got] = '\0';
	long from = got;
	for (unsigned seen = 0; from > 0; from--) {
		if (text[from - 1] == '\n' && ++seen == lines) {
			break;
		}
	}
	say(text + from);
	say("\n");
}

/* the date and time, UTC, from the seconds since 1970 (the civil calendar's days, without a library) */
void
command_date(int argc, char **argv)
{
	int64_t seconds = now_us() / 1000000, days = seconds / 86400, of_day = seconds % 86400;
	(void)argc;
	(void)argv;

	/* days since 1970-01-01 to a year, month and day (H. Hinnant's days_from_civil, backwards) */
	int64_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153, day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
	int64_t year = yoe + era * 400 + (month <= 2);
	const int64_t parts[] = { year, month, day, of_day / 3600, of_day / 60 % 60, of_day % 60 };
	const char *const after[] = { "-", "-", " ", ":", ":", " UTC\n" };

	for (int i = 0; i < 6; i++) {
		if (i > 0 && parts[i] < 10) {
			say("0");
		}
		say_number((uint64_t)parts[i]);
		say(after[i]);
	}
}
