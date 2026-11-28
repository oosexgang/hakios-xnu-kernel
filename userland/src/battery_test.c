/*
 * The battery as Darwin sees it: the kernel's power source (the IOPMPowerSource that powerd reads,
 * drivers/power/hakios.battery.cpp) and what powerd has made of it (the power sources list that
 * the status bar, the lock screen and Settings get through IOPS), side by side with the kernel's
 * own numbers (kern.hakios.battery_*).
 *
 *   battery_test            once
 *   battery_test N S        N times, S seconds apart
 *   battery_test open URL   have SpringBoard open a URL, such as prefs:root=BATTERY_USAGE (Settings >
 *                           Battery): for screenshots of a page that is hard to reach by touches alone
 *                           (needs the entitlement in battery_test.entitlements, which build.sh signs in)
 *   battery_test health     what powerd's Battery Health rests on: MobileKeyBag's "unlocked since boot" (powerd
 *                           makes its health record only then), the record itself (CFPreferences
 *                           com.apple.batteryhealthdata, by host, in /var/MobileSoftwareUpdate/Hardware/Battery) and that folder
 *   battery_test powerlog   what Darwin's power log (aggregated, CurrentPowerlog.PLSQL) has collected: every
 *                           table with rows, how many and over what time; one row of each of the accounting
 *                           tables Settings > Battery reads (the energy per application comes from them)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <glob.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* IOKit's IOPowerSources.h and IOPSKeys.h, which the SDK leaves out */
extern CFTypeRef IOPSCopyPowerSourcesInfo(void);
extern CFArrayRef IOPSCopyPowerSourcesByType(int type);         /* 1: the battery health list, Settings > Battery Health's */
extern CFArrayRef IOPSCopyPowerSourcesList(CFTypeRef blob);
extern CFDictionaryRef IOPSGetPowerSourceDescription(CFTypeRef blob, CFTypeRef ps);
extern CFStringRef IOPSGetProvidingPowerSourceType(CFTypeRef snapshot);
extern CFDictionaryRef IOPSCopyExternalPowerAdapterDetails(void);
extern double IOPSGetTimeRemainingEstimate(void);
extern int IOPSGetPercentRemaining(int *percent, bool *charging, bool *full);

/* a value in short: a number, yes or no, a string, or a dictionary of them in braces */
static void
put(char *line, size_t size, CFTypeRef value)
{
	size_t used = strlen(line);
	char text[256] = "";

	if (value == NULL) {
		snprintf(line + used, size - used, "-");
	} else if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
		snprintf(line + used, size - used, "%s", CFBooleanGetValue(value) ? "yes" : "no");
	} else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
		long long n = 0;

		CFNumberGetValue(value, kCFNumberLongLongType, &n);
		snprintf(line + used, size - used, "%lld", n);
	} else if (CFGetTypeID(value) == CFStringGetTypeID()) {
		CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8);
		snprintf(line + used, size - used, "\"%s\"", text);
	} else if (CFGetTypeID(value) == CFDictionaryGetTypeID()) {
		CFIndex count = CFDictionaryGetCount(value);
		const void **keys = calloc(count + 1, sizeof(void *)), **values = calloc(count + 1, sizeof(void *));

		CFDictionaryGetKeysAndValues(value, keys, values);
		snprintf(line + used, size - used, "{");
		for (CFIndex i = 0; i < count; i++) {
			used = strlen(line);
			text[0] = 0;
			if (CFGetTypeID(keys[i]) == CFStringGetTypeID()) {
				CFStringGetCString(keys[i], text, sizeof(text), kCFStringEncodingUTF8);
			}
			snprintf(line + used, size - used, "%s%s=", i ? ", " : "", text);
			put(line, size, values[i]);
		}
		used = strlen(line);
		snprintf(line + used, size - used, "}");
		free(keys);
		free(values);
	} else {
		snprintf(line + used, size - used, "(something else)");
	}
}

static void
show(const char *what, CFTypeRef object)
{
	static char line[16384];

	if (object == NULL) {
		printf("%s: none\n", what);
		return;
	}
	line[0] = 0;
	put(line, sizeof(line), object);
	printf("%s: %s\n", what, line);
}

static int
kernel(const char *name)
{
	int value = -1;
	size_t size = sizeof(value);

	sysctlbyname(name, &value, &size, NULL, 0);
	return value;
}

static void
once(void)
{
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("IOPMPowerSource"));
	CFTypeRef blob = IOPSCopyPowerSourcesInfo();
	CFArrayRef list = blob ? IOPSCopyPowerSourcesList(blob) : NULL;
	CFDictionaryRef adapter = IOPSCopyExternalPowerAdapterDetails();
	int percent = -1;
	bool charging = false, full = false;
	int got = IOPSGetPercentRemaining(&percent, &charging, &full);

	printf("battery_test: the kernel says %d%%, %d mV, %d mA, %d C, battery %d, charger %d, charging %d, from the DSP %d\n",
	    kernel("kern.hakios.battery_soc"), kernel("kern.hakios.battery_voltage_mv"), kernel("kern.hakios.battery_current_ma"),
	    kernel("kern.hakios.battery_temperature_c"), kernel("kern.hakios.battery_present"),
	    kernel("kern.hakios.battery_charger_present"), kernel("kern.hakios.battery_charging"),
	    kernel("kern.hakios.battery_from_dsp"));
	if (service != 0) {
		CFMutableDictionaryRef properties = NULL;
		io_name_t name = "";

		IORegistryEntryGetName(service, name);
		if (IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, 0) == KERN_SUCCESS) {
			printf("battery_test: the power source in the kernel is %s\n", name);
			show("battery_test: its properties", properties);
			CFRelease(properties);
		}
		IOObjectRelease(service);
	} else {
		printf("battery_test: there is no IOPMPowerSource in the kernel\n");
	}
	printf("battery_test: IOPSGetPercentRemaining: %d (%#x), %d%%, charging %d, full %d; time remaining %.0f s\n", got, got,
	    percent, charging, full, IOPSGetTimeRemainingEstimate());
	if (blob != NULL) {
		show("battery_test: powered by", IOPSGetProvidingPowerSourceType(blob));
	}
	for (CFIndex i = 0; list != NULL && i < CFArrayGetCount(list); i++) {
		show("battery_test: power source", IOPSGetPowerSourceDescription(blob, CFArrayGetValueAtIndex(list, i)));
	}
	if (list == NULL || CFArrayGetCount(list) == 0) {
		printf("battery_test: powerd lists no power source\n");
	}
	show("battery_test: the power adapter", adapter);
	/* what Settings > Battery > Battery Health reads: powerd's health list ("Maximum Capacity Percent") and whether
	 * the battery is genuine (AppleAuthCPRelay under the power source, isTrustedForUI) */
	CFArrayRef health = IOPSCopyPowerSourcesByType(1);
	for (CFIndex i = 0; health != NULL && i < CFArrayGetCount(health); i++) {
		show("battery_test: battery health", CFArrayGetValueAtIndex(health, i));
	}
	if (health == NULL || CFArrayGetCount(health) == 0) {
		printf("battery_test: powerd lists no battery health\n");
	}
	if (health != NULL) CFRelease(health);
	io_service_t relay = IOServiceGetMatchingService(0, IOServiceMatching("AppleAuthCPRelay"));
	if (relay != 0) {
		CFTypeRef trusted = IORegistryEntryCreateCFProperty(relay, CFSTR("isTrustedForUI"), kCFAllocatorDefault, 0);
		show("battery_test: AppleAuthCPRelay isTrustedForUI", trusted);
		if (trusted != NULL) CFRelease(trusted);
		IOObjectRelease(relay);
	} else {
		printf("battery_test: no AppleAuthCPRelay\n");
	}
	if (adapter != NULL) CFRelease(adapter);
	if (list != NULL) CFRelease(list);
	if (blob != NULL) CFRelease(blob);
	fflush(stdout);
}

/* SpringBoardServices' way of opening a URL, as a program with the entitlement may */
static int
open_url(const char *text)
{
	void *sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_NOW);
	int (*open)(CFURLRef, Boolean) = sbs ? (int (*)(CFURLRef, Boolean))dlsym(sbs, "SBSOpenSensitiveURLAndUnlock") : NULL;
	CFURLRef url = CFURLCreateWithBytes(NULL, (const UInt8 *)text, (CFIndex)strlen(text), kCFStringEncodingUTF8, NULL);
	int result;

	if (open == NULL || url == NULL) {
		printf("battery_test: cannot open %s: %s\n", text, open == NULL ? "no SBSOpenSensitiveURLAndUnlock" : "not a URL");
		return 1;
	}
	result = open(url, true);
	printf("battery_test: SpringBoard was asked to open %s: %s\n", text, result ? "it does" : "it REFUSES");
	CFRelease(url);
	return result ? 0 : 1;
}

/* the power log's database, read only (aggregated keeps writing it) */
static int
powerlog(void)
{
	glob_t found;
	sqlite3 *db;
	sqlite3_stmt *tables;

	if (glob("/private/var/containers/Shared/SystemGroup/*/Library/BatteryLife/CurrentPowerlog.PLSQL", 0, NULL, &found) != 0 ||
	    found.gl_pathc == 0) {
		printf("battery_test: no power log database (is aggregated running?)\n");
		return 1;
	}
	printf("battery_test: the power log: %s\n", found.gl_pathv[0]);
	if (sqlite3_open_v2(found.gl_pathv[0], &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		printf("battery_test: cannot open it: %s\n", sqlite3_errmsg(db));
		return 1;
	}
	sqlite3_busy_timeout(db, 2000);
	if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name", -1, &tables, NULL) != SQLITE_OK) {
		printf("battery_test: cannot list its tables: %s\n", sqlite3_errmsg(db));
		return 1;
	}
	while (sqlite3_step(tables) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(tables, 0);
		char query[512];
		sqlite3_stmt *count;
		long rows = 0;
		double first = 0, last = 0;

		snprintf(query, sizeof(query), "SELECT COUNT(*), MIN(timestamp), MAX(timestamp) FROM \"%s\"", name);
		if (sqlite3_prepare_v2(db, query, -1, &count, NULL) != SQLITE_OK) {
			snprintf(query, sizeof(query), "SELECT COUNT(*), 0, 0 FROM \"%s\"", name);
			if (sqlite3_prepare_v2(db, query, -1, &count, NULL) != SQLITE_OK) {
				continue;
			}
		}
		if (sqlite3_step(count) == SQLITE_ROW) {
			rows = sqlite3_column_int64(count, 0);
			first = sqlite3_column_double(count, 1);
			last = sqlite3_column_double(count, 2);
		}
		sqlite3_finalize(count);
		if (rows == 0) {
			continue;
		}
		printf("battery_test: powerlog %-70s %6ld rows, %.0f s from the first to the last\n", name, rows, last - first);
		if (strncmp(name, "PLAccountingOperator", 20) == 0 || strncmp(name, "PLBatteryAgent_EventBackward_Battery", 36) == 0) {
			sqlite3_stmt *row;

			snprintf(query, sizeof(query), "SELECT * FROM \"%s\" ORDER BY ID DESC LIMIT 1", name);
			if (sqlite3_prepare_v2(db, query, -1, &row, NULL) == SQLITE_OK) {
				if (sqlite3_step(row) == SQLITE_ROW) {
					char line[1024] = "";

					for (int c = 0; c < sqlite3_column_count(row) && strlen(line) < sizeof(line) - 80; c++) {
						const unsigned char *v = sqlite3_column_text(row, c);

						snprintf(line + strlen(line), sizeof(line) - strlen(line), " %s=%.40s", sqlite3_column_name(row, c),
						    v ? (const char *)v : "-");
					}
					printf("battery_test: powerlog   the last row:%s\n", line);
				}
				sqlite3_finalize(row);
			}
		}
	}
	sqlite3_finalize(tables);
	sqlite3_close(db);
	globfree(&found);
	return 0;
}

/* a folder and what is under it, a few levels down */
static void
list(const char *path, int depth)
{
	DIR *dir = opendir(path);
	struct dirent *e;
	char sub[1024];
	struct stat st;

	if (dir == NULL) {
		printf("battery_test:   %*s%s: cannot open\n", depth * 2, "", path);
		return;
	}
	while ((e = readdir(dir)) != NULL) {
		if (e->d_name[0] == '.') continue;
		snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
		if (lstat(sub, &st) != 0) continue;
		printf("battery_test:   %*s%s %o %u:%u %lld\n", depth * 2, "", sub, st.st_mode & 07777, st.st_uid, st.st_gid,
		    (long long)st.st_size);
		if (S_ISDIR(st.st_mode) && depth < 6) list(sub, depth + 1);
	}
	closedir(dir);
}

static int
health(void)
{
	int (*unlocked)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "MKBDeviceUnlockedSinceBoot");
	int (*state)(CFDictionaryRef) = (int (*)(CFDictionaryRef))dlsym(RTLD_DEFAULT, "MKBGetDeviceLockState");
	extern CFDictionaryRef _CFPreferencesCopyMultipleWithContainer(CFArrayRef, CFStringRef, CFStringRef, CFStringRef,
	    CFStringRef);
	char line[2048] = "";
	CFDictionaryRef record;

	if (unlocked == NULL) {
		dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag", RTLD_NOW);
		unlocked = (int (*)(void))dlsym(RTLD_DEFAULT, "MKBDeviceUnlockedSinceBoot");
		state = (int (*)(CFDictionaryRef))dlsym(RTLD_DEFAULT, "MKBGetDeviceLockState");
	}
	printf("battery_test: MKBDeviceUnlockedSinceBoot %d, MKBGetDeviceLockState %d\n", unlocked ? unlocked() : -99,
	    state ? state(NULL) : -99);
	record = _CFPreferencesCopyMultipleWithContainer(NULL, CFSTR("com.apple.batteryhealthdata"), kCFPreferencesCurrentUser,
	    kCFPreferencesCurrentHost, CFSTR("/var/MobileSoftwareUpdate/Hardware/Battery/"));
	if (record) put(line, sizeof(line), record);
	printf("battery_test: health record: %s\n", record ? line : "none");
	fflush(stdout);
	list("/var/MobileSoftwareUpdate", 0);
	/* who would write it: powerd asks cfprefsd, which writes the file; their users, the host's UUID (ByHost) */
	{
		int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
		size_t size = 0;
		struct kinfo_proc *procs;

		if (sysctl(mib, 3, NULL, &size, NULL, 0) == 0 && (procs = malloc(size)) != NULL) {
			if (sysctl(mib, 3, procs, &size, NULL, 0) == 0) {
				for (size_t i = 0; i < size / sizeof(*procs); i++) {
					const char *name = procs[i].kp_proc.p_comm;
					if (strcmp(name, "powerd") == 0 || strcmp(name, "cfprefsd") == 0) {
						printf("battery_test: %s pid %d uid %u\n", name, procs[i].kp_proc.p_pid,
						    procs[i].kp_eproc.e_ucred.cr_uid);
					}
				}
			}
			free(procs);
		}
	}
	{
		uuid_t host;
		struct timespec wait = { 1, 0 };
		uuid_string_t text = "-";
		int (*hostuuid)(uuid_t, const struct timespec *) = (int (*)(uuid_t, const struct timespec *))dlsym(RTLD_DEFAULT,
		    "gethostuuid");
		int rc = hostuuid ? hostuuid(host, &wait) : -99;

		if (rc == 0) uuid_unparse(host, text);
		printf("battery_test: gethostuuid %d %s\n", rc, text);
	}
	{
		extern void _CFPreferencesSetValueWithContainer(CFStringRef, CFPropertyListRef, CFStringRef, CFStringRef,
		    CFStringRef, CFStringRef);
		extern Boolean _CFPreferencesSynchronizeWithContainer(CFStringRef, CFStringRef, CFStringRef, CFStringRef);
		CFStringRef where = CFSTR("/var/MobileSoftwareUpdate/Hardware/Battery/");
		Boolean synced;

		_CFPreferencesSetValueWithContainer(CFSTR("probe"), CFSTR("written"), CFSTR("com.hakios.prefsprobe"),
		    kCFPreferencesCurrentUser, kCFPreferencesCurrentHost, where);
		synced = _CFPreferencesSynchronizeWithContainer(CFSTR("com.hakios.prefsprobe"), kCFPreferencesCurrentUser,
		    kCFPreferencesCurrentHost, where);
		printf("battery_test: a ByHost value written in that container: synchronized %d\n", synced);
		fflush(stdout);
		list("/var/MobileSoftwareUpdate", 0);
	}
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "health") == 0) {
		return health();
	}
	if (argc > 2 && strcmp(argv[1], "open") == 0) {
		return open_url(argv[2]);
	}
	if (argc > 1 && strcmp(argv[1], "powerlog") == 0) {
		return powerlog();
	}
	int times = argc > 1 ? atoi(argv[1]) : 1;
	int seconds = argc > 2 ? atoi(argv[2]) : 10;

	for (int i = 0; i < times; i++) {
		if (i > 0) sleep(seconds);
		once();
	}
	return 0;
}
