/*
 * wifi_client: Wi-Fi run by wifid, asked the way Settings asks it -- MobileWiFi's client calls, which go to wifid,
 * which asks the kernel in Apple80211 requests (drivers/wifi/hakios.apple80211.c). So what this proves is wifid's:
 * the radio's power, a scan wifid has made and what it heard, a join wifid has made.
 *
 * usage: wifi_client STEP...
 *   power        wifid turns the radio on (WiFiManagerClientSetPower)
 *   scan         wifid scans (WiFiDeviceClientScanAsync); each network it heard: name, channel, signal
 *   join         the network of /redwood/fw/wlan.conf (two lines, ssid= and psk=; userland/build.sh), looked for in
 *                the last scan, given its password (WiFiNetworkSetPassword: wifid's keychain) and joined by wifid
 *                (WiFiDeviceClientAssociateAsync), then kept as a known network (WiFiManagerClientAddNetwork)
 *   wait S
 * Every line is written into the kernel's log as well (sysctl kern.hakios_say): the console drops lines. The
 * password is never printed; the name of the network that is wanted is not printed either.
 * Signed with wifi_client.entitlements (wifid's own rights, as wifid and the keychain check a client by those).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

typedef void *ref;
typedef void (*scan_done)(ref device, CFArrayRef networks, int error, void *context);
typedef void (*join_done)(ref device, ref network, CFDictionaryRef info, int error, void *context);

static ref (*manager_create)(CFAllocatorRef, int);
static void (*manager_schedule)(ref, CFRunLoopRef, CFStringRef);
static CFArrayRef (*manager_devices)(ref);
static int (*manager_set_power)(ref, Boolean);
static Boolean (*manager_get_power)(ref);
static int (*manager_add_network)(ref, ref);
static int (*device_scan)(ref, CFDictionaryRef, scan_done, void *);
static int (*device_join)(ref, ref, join_done, void *);
static CFStringRef (*network_ssid)(ref);
static CFTypeRef (*network_property)(ref, CFStringRef);
static void (*network_set_password)(ref, CFStringRef);

static ref manager, device, wanted_network;
static char wanted[33];
static unsigned wanted_length;
static bool done;
static int last_error;

static void
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("%s\n", line);
	sysctlbyname("kern.hakios_say", NULL, NULL, line, strlen(line) + 1);
}

static int
number(ref network, CFStringRef key)
{
	CFTypeRef value = network_property(network, key);
	int n = 0;

	if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
		CFNumberGetValue(value, kCFNumberIntType, &n);
	}
	return n;
}

/* the network of /redwood/fw/wlan.conf: its name kept here, its password handed straight to the network */
static CFStringRef
read_conf(void)
{
	char buffer[256], *line, *next;
	CFStringRef password = NULL;
	int fd = open("/redwood/fw/wlan.conf", O_RDONLY);
	ssize_t length = fd >= 0 ? read(fd, buffer, sizeof(buffer) - 1) : -1;

	if (fd >= 0) {
		close(fd);
	}
	if (length <= 0) {
		return NULL;
	}
	buffer[length] = 0;
	for (line = buffer; line != NULL; line = next) {
		next = strchr(line, '\n');
		if (next != NULL) {
			*next++ = 0;
		}
		if (*line != 0 && line[strlen(line) - 1] == '\r') {
			line[strlen(line) - 1] = 0;
		}
		if (strncmp(line, "ssid=", 5) == 0 && strlen(line + 5) <= 32) {
			wanted_length = (unsigned)strlen(line + 5);
			memcpy(wanted, line + 5, wanted_length);
		} else if (strncmp(line, "psk=", 4) == 0) {
			password = CFStringCreateWithCString(NULL, line + 4, kCFStringEncodingUTF8);
		}
	}
	memset(buffer, 0, sizeof(buffer));
	return password;
}

static void
scanned(ref d, CFArrayRef networks, int error, void *context)
{
	CFIndex count = networks != NULL ? CFArrayGetCount(networks) : 0;

	(void)d;
	(void)context;
	last_error = error;
	say("wifi_client: wifid's scan: %ld networks, error %d", (long)count, error);
	for (CFIndex i = 0; i < count; i++) {
		ref network = (ref)CFArrayGetValueAtIndex(networks, i);
		CFStringRef ssid = network_ssid(network);
		char name[80] = "";
		bool is_wanted;

		if (ssid != NULL) {
			CFStringGetCString(ssid, name, sizeof(name), kCFStringEncodingUTF8);
		}
		is_wanted = wanted_length != 0 && strlen(name) == wanted_length && memcmp(name, wanted, wanted_length) == 0;
		if (is_wanted && wanted_network == NULL) {
			wanted_network = network;
			CFRetain(network);
		}
		say("wifi_client:   heard by wifid: channel %d, %d dBm, \"%s\"", number(network, CFSTR("CHANNEL")),
		    number(network, CFSTR("RSSI")), is_wanted ? "(the network that is wanted)" : name);
	}
	done = true;
	CFRunLoopStop(CFRunLoopGetCurrent());
}

static void
joined(ref d, ref network, CFDictionaryRef info, int error, void *context)
{
	(void)d;
	(void)network;
	(void)info;
	(void)context;
	last_error = error;
	say("wifi_client: wifid's join: %s (error %d)", error == 0 ? "JOINED" : "FAILED", error);
	done = true;
	CFRunLoopStop(CFRunLoopGetCurrent());
}

static void
run_until_done(double seconds)
{
	done = false;
	for (double t = 0; !done && t < seconds; t += 1) {
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1, false);
	}
}

#define FIND(variable, name) (*(void **)&variable = dlsym(mw, name))

int
main(int argc, char **argv)
{
	void *mw = dlopen("/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi", RTLD_NOW);
	CFArrayRef devices;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (mw == NULL || !FIND(manager_create, "WiFiManagerClientCreate") || !FIND(manager_schedule,
	    "WiFiManagerClientScheduleWithRunLoop") || !FIND(manager_devices, "WiFiManagerClientCopyDevices") ||
	    !FIND(manager_set_power, "WiFiManagerClientSetPower") || !FIND(manager_get_power, "WiFiManagerClientGetPower") ||
	    !FIND(manager_add_network, "WiFiManagerClientAddNetwork") || !FIND(device_scan, "WiFiDeviceClientScanAsync") ||
	    !FIND(device_join, "WiFiDeviceClientAssociateAsync") || !FIND(network_ssid, "WiFiNetworkGetSSID") ||
	    !FIND(network_property, "WiFiNetworkGetProperty") || !FIND(network_set_password, "WiFiNetworkSetPassword")) {
		say("wifi_client: MobileWiFi or one of its calls is not there: %s", dlerror());
		return 1;
	}
	/* waits that come first are made before wifid is asked anything: it registers its service a while into the start */
	while (argc > 2 && strcmp(argv[1], "wait") == 0) {
		usleep((useconds_t)(atof(argv[2]) * 1e6));
		argc -= 2;
		argv += 2;
	}
	for (int tries = 0; tries < 30; tries++) {
		CFArrayRef some;

		if ((manager = manager_create(kCFAllocatorDefault, 0)) == NULL) {
			sleep(2);
			continue;
		}
		manager_schedule(manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
		some = manager_devices(manager);
		if (some != NULL && CFArrayGetCount(some) != 0) {
			CFRelease(some);
			break;
		}
		if (some != NULL) {
			CFRelease(some);
		}
		CFRelease(manager);
		manager = NULL;
		sleep(2);
	}
	if (manager == NULL) {
		say("wifi_client: wifid shows no Wi-Fi device in 60 s (wifid not there, or this program not allowed)");
		return 1;
	}
	devices = manager_devices(manager);
	say("wifi_client: wifid has %ld Wi-Fi device(s); the power is %s", devices ? (long)CFArrayGetCount(devices) : 0L,
	    manager_get_power(manager) ? "on" : "off");
	if (devices == NULL || CFArrayGetCount(devices) == 0) {
		return 1;
	}
	device = (ref)CFArrayGetValueAtIndex(devices, 0);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "power") == 0) {
			say("wifi_client: wifid turns the power on: %d; now %s", manager_set_power(manager, true),
			    manager_get_power(manager) ? "on" : "off");
		} else if (strcmp(argv[i], "scan") == 0) {
			CFDictionaryRef options = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
			    &kCFTypeDictionaryValueCallBacks);
			CFStringRef password = read_conf();	/* (for the name of the network that is wanted, not printed) */
			int error;

			if (password != NULL) {
				CFRelease(password);
			}
			if (wanted_network != NULL) {
				CFRelease(wanted_network);
				wanted_network = NULL;
			}
			error = device_scan(device, options, scanned, NULL);
			say("wifi_client: wifid is asked to scan: %d", error);
			if (error == 0) {
				run_until_done(40);
				if (!done) {
					say("wifi_client: wifid's scan: NO ANSWER in 40 s");
				}
			}
			CFRelease(options);
		} else if (strcmp(argv[i], "join") == 0) {
			CFStringRef password = read_conf();
			int error;

			if (password == NULL || wanted_length == 0) {
				say("wifi_client: join: /redwood/fw/wlan.conf has no network");
				continue;
			}
			if (wanted_network == NULL) {
				say("wifi_client: join: the network that is wanted was not in wifid's last scan");
				CFRelease(password);
				continue;
			}
			network_set_password(wanted_network, password);
			CFRelease(password);
			error = device_join(device, wanted_network, joined, NULL);
			say("wifi_client: wifid is asked to join the network that is wanted: %d", error);
			if (error == 0) {
				run_until_done(60);
				if (!done) {
					say("wifi_client: wifid's join: NO ANSWER in 60 s");
				}
			}
			if (done && last_error == 0) {
				say("wifi_client: kept as a known network: %d", manager_add_network(manager, wanted_network));
			}
		} else if (strcmp(argv[i], "wait") == 0 && i + 1 < argc) {
			usleep((useconds_t)(atof(argv[++i]) * 1e6));
		} else {
			say("wifi_client: bad step \"%s\"", argv[i]);
			return 1;
		}
	}
	return 0;
}
