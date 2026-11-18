/*
 * Darwin's Wi-Fi requests (drivers/board/apple80211.c), run as a host program: SIOCGA80211 and
 * SIOCSA80211 as wifid sends them, answered from the station layer -- here a model of it that answers as a
 * scenario says, and whose records come from a radio's driver built outside this tree, so may say more
 * than there is room for. What goes back into the caller's buffer never passes the length it gave, and
 * what a radio says is held to the room in Apple's layouts.
 *
 *   apple80211_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include "fakekernel.h"

extern char log_text[];
#define KERNEL 1
#include <sys/board/sta.h>

/* ---- the station layer ------------------------------------------------------------------------------- */

static struct hakios_sta_bss heard[4];
static unsigned heard_n;
static struct hakios_sta_state state = { .phase = HAKIOS_STA_IDLE };
static struct hakios_sta_join asked_join;
static struct hakios_sta_scan asked_scan;
static unsigned joins, scans, leaves;

bool hakios_sta_present(void) { return true; }
static bool powered = true;
bool hakios_sta_powered(void) { return powered; }
static int power_asked = -1;              /* (what the last power request asked for: 1 on, 0 off) */
int hakios_sta_ask_power(bool on) { power_asked = on; return 0; }
int hakios_sta_ask_leave(void) { leaves++; return 0; }
static bool join_by_darwin = true;         /* (whether the join told over is one Darwin asked for) */
bool hakios_sta_join_by_darwin(void) { return join_by_darwin; }
void hakios_sta_state(struct hakios_sta_state *out) { *out = state; }

bool
hakios_sta_heard(unsigned index, struct hakios_sta_bss *bss)
{
	if (index >= heard_n) {
		return false;
	}
	*bss = heard[index];
	return true;
}

unsigned
hakios_sta_channels(struct hakios_sta_channel *channels, unsigned most)
{
	static const uint16_t frequencies[] = { 2412, 2437, 5180, 5500 };
	unsigned n = 0;

	for (; n < 4 && n < most; n++) {
		channels[n].frequency = frequencies[n];
		channels[n].flags = frequencies[n] == 5500 ? HAKIOS_STA_CHANNEL_RADAR : frequencies[n] == 5180 ? HAKIOS_STA_CHANNEL_PASSIVE : 0;
	}
	return n;
}

int
hakios_sta_ask_join(const struct hakios_sta_join *what)
{
	asked_join = *what;
	joins++;
	return 0;
}

int
hakios_sta_ask_scan(const struct hakios_sta_scan *what)
{
	asked_scan = *what;
	scans++;
	return 0;
}

/* what the driver prints (the kernel's printf: its console, not the loader's log) */
static char printed[1 << 15];

static __printflike(1, 2) int
driver_printf(const char *format, ...)
{
	size_t length = strlen(printed);
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(printed + length, sizeof(printed) - length, format, arguments);
	va_end(arguments);
	return 0;
}

static bool
printed_has(const char *text)
{
	return strstr(printed, text) != NULL;
}

/* (the fake kernel's get and set are the sysctls' helpers; the driver has its own) */
#include <sys/board/input.h>                  /* (before printf is the test's: they name it in a format attribute) */
#include <sys/board/net.h>
#define get apple80211_get
#define set apple80211_set
#define printf driver_printf
#include "board/apple80211.c"
#undef get
#undef set
#undef printf

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- the caller --------------------------------------------------------------------------------------- */

#define FILL 0x5a

static int
request(bool is_set, int32_t type, void *buffer, uint32_t length, struct apple80211req *out)
{
	struct apple80211req request = { .req_if_name = "en0", .req_type = type, .req_len = length,
		                         .req_data = (user_addr_t)(uintptr_t)buffer };
	int error = hakios_apple80211_ioctl(is_set ? SIOCSA80211 : SIOCGA80211, &request);

	if (out != NULL) {
		*out = request;
	}
	return error;
}

/* the bytes from `from` on are as the caller left them */
static bool
untouched(const void *buffer, size_t from, size_t size)
{
	for (size_t i = from; i < size; i++) {
		if (((const uint8_t *)buffer)[i] != FILL) {
			return false;
		}
	}
	return true;
}

/* a network the radio heard, saying more than there is room for when `wild` */
static struct hakios_sta_bss
network(const char *name, bool wild)
{
	struct hakios_sta_bss bss = { .bssid = { 2, 4, 6, 8, 10, 12 }, .frequency = 5180, .rssi = -55, .beacon_interval = 100 };

	bss.ssid_length = (uint8_t)strlen(name);
	memcpy(bss.ssid, name, bss.ssid_length);
	/* the rates element: 6, 12 and 24 Mbit/s, then one that is not a rate */
	bss.elements[0] = 1;
	bss.elements[1] = 4;
	bss.elements[2] = 0x8c;
	bss.elements[3] = 0x18;
	bss.elements[4] = 0xb0;
	bss.elements[5] = 0xff;
	bss.elements_length = 6;
	if (wild) {
		bss.ssid_length = 0xff;
		bss.elements_length = 0xffff;
	}
	return bss;
}

/* ---- scenarios ---------------------------------------------------------------------------------------- */

/* a scan's networks one a request, as the layout has them, and EIO once they are all given */
static void
scan_results(void)
{
	struct apple80211_scan_result result;

	heard[heard_n++] = network("home", false);
	heard[heard_n++] = network("cafe", false);
	for (unsigned i = 0; i < 2; i++) {
		memset(&result, FILL, sizeof(result));
		result.elements_length = sizeof(result.elements);
		CHECK(request(false, 11, &result, sizeof(result), NULL) == 0, "network %u given", i);
		CHECK(result.ssid_length == 4 && memcmp(result.ssid, i == 0 ? "home" : "cafe", 4) == 0, "network %u's name", i);
		CHECK(result.channel.channel == 36 && (result.channel.flags & CHANNEL_5GHZ), "on channel 36: %u %#x", result.channel.channel,
		    result.channel.flags);
		CHECK(result.rate_count == 3 && result.rates[0] == 6 && result.rates[1] == 12 && result.rates[2] == 24,
		    "the rates it has, the one that is not left out: %u", result.rate_count);
		CHECK(result.elements_length == 6 && result.elements[0] == 1, "the elements as heard: %u", result.elements_length);
		CHECK(result.rssi == -55 && result.bssid[5] == 12, "how strongly and from where");
	}
	CHECK(request(false, 11, &result, sizeof(result), NULL) == EIO, "that was all");
	/* a scan that is over starts the list again, and Darwin is told */
	hakios_apple80211_event(HAKIOS_STA_SCAN_DONE, 0);
	CHECK(fake_events == 1 && fake_last_event.kev_class == KEV_IEEE80211_CLASS && fake_last_event.kev_subclass == 1 &&
	    fake_last_event.event_code == EVENT_SCAN_DONE && fake_last_event.dv[0].data_length == 16,
	    "a scan's end said as Apple80211's event 10: %u events, code %u", fake_events, fake_last_event.event_code);
	CHECK(request(false, 11, &result, sizeof(result), NULL) == 0 && memcmp(result.ssid, "home", 4) == 0, "from the first again");
}

/* a record that says more than there is room for: the name and the elements held to the layout's room */
static void
radio_says_too_much(void)
{
	struct apple80211_scan_result result;

	heard[heard_n++] = network("x", true);
	memset(&result, FILL, sizeof(result));
	result.elements_length = sizeof(result.elements);
	CHECK(request(false, 11, &result, sizeof(result), NULL) == 0, "given");
	CHECK(result.ssid_length == 32, "a name of 32 bytes at most: %u", result.ssid_length);
	CHECK(result.elements_length == sizeof(result.elements), "elements as many as there is room for: %u", result.elements_length);
	CHECK(fake_copied_out == sizeof(result), "the layout's length copied out: %zu", fake_copied_out);

	/* and the network joined, asked for by its name: 32 bytes, and the length given back says so */
	uint8_t name[64];
	struct apple80211req answered;

	state.phase = HAKIOS_STA_JOINED;
	state.bss = heard[0];
	memset(name, FILL, sizeof(name));
	CHECK(request(false, 1, name, sizeof(name), &answered) == 0, "the name given");
	CHECK(answered.req_len == 32 && untouched(name, 32, sizeof(name)), "32 bytes and no more: %u", answered.req_len);
}

/* the caller says how much room it has for elements; what does not fit is not given at all, rather than cut */
static void
caller_room(void)
{
	struct apple80211_scan_result result;

	heard[heard_n++] = network("home", false);
	heard[heard_n++] = network("home", false);
	memset(&result, FILL, sizeof(result));
	result.elements_length = 5;
	CHECK(request(false, 11, &result, sizeof(result), NULL) == 0, "given");
	CHECK(result.elements_length == 0, "six bytes of elements do not fit in five: %u", result.elements_length);
	CHECK(result.rate_count == 3, "the rates read all the same: %u", result.rate_count);
	memset(&result, FILL, sizeof(result));
	result.elements_length = 6;
	CHECK(request(false, 11, &result, sizeof(result), NULL) == 0 && result.elements_length == 6, "they fit in six: %u",
	    result.elements_length);
}

/* a buffer shorter than the answer: as much as it has room for, and not a byte past it */
static void
short_buffer(void)
{
	static uint8_t buffer[sizeof(struct apple80211_scan_result)];
	struct apple80211req answered;

	heard[heard_n++] = network("home", false);
	memset(buffer, FILL, sizeof(buffer));
	CHECK(request(false, 11, buffer, 100, NULL) == 0, "given");
	CHECK(fake_copied_out == 100 && untouched(buffer, 100, sizeof(buffer)), "100 bytes: %zu", fake_copied_out);
	memset(buffer, FILL, sizeof(buffer));
	CHECK(request(false, 27, buffer, 8, NULL) == 0, "the channels");
	CHECK(fake_copied_out == 8 && untouched(buffer, 8, sizeof(buffer)) && buffer[4] == 4, "8 bytes, saying 4 channels: %zu",
	    fake_copied_out);
	CHECK(request(false, 43, NULL, 256, &answered) == 0, "no buffer at all: nothing copied, no fault");
	CHECK(request(false, 15, NULL, 0, &answered) == 0 && answered.req_val == 1, "the mode, in the value: %d", answered.req_val);
}

/* a join: the name, and the passphrase as typed; lengths that do not fit refused; nothing of either in the log */
static void
join(void)
{
	struct apple80211_assoc assoc;
	struct apple80211req answered;

	memset(&assoc, 0, sizeof(assoc));
	assoc.ssid_length = 4;
	memcpy(assoc.ssid, "home", 4);
	assoc.key_cipher = CIPHER_PASSPHRASE;
	assoc.key_length = 10;
	memcpy(assoc.key, "secretpass", 10);
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == 0, "asked");
	CHECK(joins == 1 && asked_join.security == HAKIOS_STA_WPA2_PSK && asked_join.passphrase_length == 10 &&
	    memcmp(asked_join.passphrase, "secretpass", 10) == 0 && asked_join.ssid_length == 4, "WPA2 with the passphrase");
	CHECK(!printed_has("secretpass") && !printed_has("home"), "neither the name nor the key in the log: %s", printed);
	CHECK(printed_has("a name of 4 bytes, any access point"), "lengths only: %s", printed);

	assoc.key_length = 7;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == EINVAL, "a passphrase of 7 bytes");
	assoc.key_length = 64;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == EINVAL, "and of 64");
	assoc.key_length = 8;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == 0 && asked_join.passphrase_length == 8, "but of 8");
	assoc.key_length = 63;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == 0 && asked_join.passphrase_length == 63, "and of 63");
	assoc.key_cipher = CIPHER_PMK;
	assoc.key_length = 31;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == EINVAL, "a key that is not 32 bytes");
	assoc.key_cipher = CIPHER_PASSPHRASE;
	assoc.key_length = 10;
	assoc.ssid_length = 33;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == EINVAL, "a name of 33 bytes");
	assoc.ssid_length = 32;
	memset(assoc.ssid, 'n', 32);
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == 0 && asked_join.ssid_length == 32, "a name of 32 bytes, the longest");
	joins--;                                /* (not one of the four below) */
	memcpy(assoc.ssid, "home", 4);
	assoc.ssid_length = 0;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == EINVAL, "no name");
	assoc.ssid_length = 4;
	CHECK(request(true, 20, &assoc, offsetof(struct apple80211_assoc, rest) - 1, NULL) == EINVAL, "a request cut short");
	assoc.key_cipher = CIPHER_PMK;
	assoc.key_length = 32;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == 0 && asked_join.passphrase_length == 0 &&
	    asked_join.security == HAKIOS_STA_WPA2_PSK, "the key itself");
	CHECK(joins == 4, "nothing refused reached the station (four taken): %u", joins);

	/* how it went, in Apple80211's numbers */
	static const struct { int status; int32_t result; } outcomes[] = { { 0, 1 }, { EAUTH, 21 }, { ETIMEDOUT, 9 }, { EIO, 2 } };
	for (unsigned i = 0; i < 4; i++) {
		hakios_apple80211_event(HAKIOS_STA_JOIN_DONE, outcomes[i].status);
		CHECK(request(false, 21, NULL, 0, &answered) == 0 && answered.req_val == outcomes[i].result, "%d: %d, not %d",
		    outcomes[i].status, answered.req_val, outcomes[i].result);
		CHECK(fake_last_event.event_code == EVENT_ASSOC_DONE, "event 9 posted");
	}
}

/* the link, as Darwin is told of it: the name and the access point changed, then the link's own event -- up with the
 * station's RSSI, down when asked, down and gone (involuntary, reason 4) otherwise */
static void
link_changes(void)
{
	static const struct { int status; uint8_t down, involuntary; uint32_t reason; int32_t rssi; } cases[] = {
		{ 1, 0, 0, 0, -55 }, { 0, 1, 0, 0, 0 }, { ETIMEDOUT, 1, 1, 4, 0 }, { EIO, 1, 1, 4, 0 },
	};

	state.bss.rssi = -55;
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct apple80211_link_changed link;
		unsigned before = fake_events;

		hakios_apple80211_event(HAKIOS_STA_LINK, cases[i].status);
		CHECK(fake_events == before + 3 && fake_event_codes[before % 16] == EVENT_SSID_CHANGED &&
		    fake_event_codes[(before + 1) % 16] == EVENT_BSSID_CHANGED && fake_event_codes[(before + 2) % 16] == EVENT_LINK_CHANGED,
		    "%d: the name, the access point, then the link (%u events)", cases[i].status, fake_events - before);
		/* (the interface's name first, then the link's) */
		CHECK(fake_last_event_length == 16 + sizeof(link), "%d: the name and the link carried: %u bytes", cases[i].status,
		    fake_last_event_length);
		memcpy(&link, fake_last_event_data + 16, sizeof(link));
		CHECK(link.down == cases[i].down && link.involuntary == cases[i].involuntary && link.reason == cases[i].reason &&
		    link.rssi == cases[i].rssi, "%d: down %u involuntary %u reason %u rssi %d", cases[i].status, link.down, link.involuntary,
		    link.reason, link.rssi);
	}
	hakios_apple80211_event(HAKIOS_STA_SCAN_DONE, 0);
	CHECK(fake_last_event.event_code == EVENT_SCAN_DONE && fake_last_event_length == 16, "an event without data: the name alone");
}

/* a scan asked for: the channels named, or every channel when more are named than there is room for */
static void
scan_request(void)
{
	static struct apple80211_scan_request scan;

	memset(&scan, 0, sizeof(scan));
	scan.scan_type = 2;
	scan.channel_count = 2;
	scan.channels[0] = (struct apple80211_channel) { 1, 1, CHANNEL_2GHZ };
	scan.channels[1] = (struct apple80211_channel) { 1, 36, CHANNEL_5GHZ };
	scan.ssid_length = 4;
	memcpy(scan.ssid, "home", 4);
	CHECK(request(true, 10, &scan, sizeof(scan), NULL) == 0, "asked");
	CHECK(asked_scan.passive && asked_scan.frequency_count == 2 && asked_scan.frequencies[0] == 2412 &&
	    asked_scan.frequencies[1] == 5180, "listening, on 2412 and 5180: %u", asked_scan.frequency_count);
	CHECK(asked_scan.ssid_length == 4, "for the network named");

	scan.ssid_length = 33;
	scan.channel_count = 65;
	CHECK(request(true, 10, &scan, sizeof(scan), NULL) == 0, "asked");
	CHECK(asked_scan.frequency_count == 0 && asked_scan.ssid_length == 0, "every channel, for any name: %u",
	    asked_scan.frequency_count);

	/* as many as there is room for, and the longest name: each taken */
	scan.ssid_length = 32;
	memset(scan.ssid, 'n', 32);
	scan.channel_count = 64;
	for (unsigned i = 0; i < 64; i++) {
		scan.channels[i] = (struct apple80211_channel) { 1, i < 13 ? 1 + i : 36 + 4 * (i - 13) > 196 ? 36 : 36 + 4 * (i - 13),
			                                         i < 13 ? CHANNEL_2GHZ : CHANNEL_5GHZ };
	}
	scan.channels[63] = (struct apple80211_channel) { 1, 0, 0 };    /* (and one the radio has not, at the end) */
	CHECK(request(true, 10, &scan, sizeof(scan), NULL) == 0, "asked");
	CHECK(asked_scan.frequency_count == 63 && asked_scan.ssid_length == 32, "64 channels, one not the radio's: 63 (%u); a name of 32",
	    asked_scan.frequency_count);

	scan.channel_count = 1;
	scan.channels[0] = (struct apple80211_channel) { 1, 0, 0 };
	CHECK(request(true, 10, &scan, sizeof(scan), NULL) == EINVAL, "only a channel the radio has not");
	CHECK(request(true, 10, &scan, sizeof(scan) - 1, NULL) == EINVAL, "a request cut short");
	CHECK(scans == 3, "nothing refused reached the station: %u", scans);
}

/* channels by number alone, with no band said: 1 to 14 are 2.4 GHz (14 is 2484 MHz), the rest 5 GHz up to 196; 6 GHz none */
static void
channels_by_number(void)
{
	static struct apple80211_scan_request scan;
	static const struct { unsigned channel, flags, frequency; } asked[] = {
		{ 14, 0, 2484 }, { 13, 0, 2472 }, { 196, 0, 5980 }, { 197, 0, 0 }, { 36, 0, 5180 }, { 1, 0x2000, 0 }, { 0, 0, 0 },
		{ 14, CHANNEL_5GHZ, 5070 },
	};
	unsigned n = 0;

	memset(&scan, 0, sizeof(scan));
	scan.channel_count = sizeof(asked) / sizeof(asked[0]);
	for (unsigned i = 0; i < scan.channel_count; i++) {
		scan.channels[i] = (struct apple80211_channel) { 1, asked[i].channel, asked[i].flags };
	}
	CHECK(request(true, 10, &scan, sizeof(scan), NULL) == 0, "asked");
	for (unsigned i = 0; i < scan.channel_count; i++) {
		if (asked[i].frequency != 0) {
			CHECK(n < asked_scan.frequency_count && asked_scan.frequencies[n] == asked[i].frequency, "channel %u (%#x): %u MHz",
			    asked[i].channel, asked[i].flags, n < asked_scan.frequency_count ? asked_scan.frequencies[n] : 0);
			n++;
		}
	}
	CHECK(asked_scan.frequency_count == n, "%u of them the radio's: %u", n, asked_scan.frequency_count);
}

/* rates from both of the elements that carry them (1 and 50), wherever they are among the others */
static void
rates_in_two_elements(void)
{
	static const uint8_t elements[] = { 0, 4, 'h', 'o', 'm', 'e', 1, 2, 0x82, 0x84, 3, 1, 6, 50, 2, 0x8c, 0x98 };
	struct hakios_sta_bss bss = network("home", false);
	struct apple80211_scan_result result;

	memcpy(bss.elements, elements, sizeof(elements));
	bss.elements_length = sizeof(elements);
	heard[heard_n++] = bss;
	memset(&result, FILL, sizeof(result));
	result.elements_length = sizeof(result.elements);
	CHECK(request(false, 11, &result, sizeof(result), NULL) == 0, "given");
	CHECK(result.rate_count == 4 && result.rates[0] == 1 && result.rates[1] == 2 && result.rates[2] == 6 && result.rates[3] == 12,
	    "1, 2, 6 and 12 Mbit/s: %u rates", result.rate_count);
}

/* 54 Mbit/s, the fastest a rate element says, is one; more than fifteen rates are fifteen */
static void
the_most_rates(void)
{
	uint8_t elements[2 + 8 + 2 + 10] = { 1, 8, 0x6c, 0xee, 0x02, 0x04, 0x0b, 0x16, 0x0c, 0x12, 50, 10 };
	struct hakios_sta_bss bss = network("home", false);
	struct apple80211_scan_result result;

	/* (54 Mbit/s -- 108 halves -- counts; 0xee, a basic rate of 55, is none of Wi-Fi's: not taken) */
	for (unsigned i = 0; i < 10; i++) {
		elements[12 + i] = (uint8_t)(0x18 + 2 * i);     /* (12 to 21 Mbit/s: ten more) */
	}
	memcpy(bss.elements, elements, sizeof(elements));
	bss.elements_length = sizeof(elements);
	heard[heard_n++] = bss;
	memset(&result, FILL, sizeof(result));
	result.elements_length = sizeof(result.elements);
	CHECK(request(false, 11, &result, sizeof(result), NULL) == 0, "given");
	CHECK(result.rate_count == 15 && result.rates[0] == 54, "fifteen of the seventeen, the first 54 Mbit/s: %u, %u", result.rate_count,
	    result.rates[0]);
}

/* an element that says it is longer than what is left of them: not read, nor what lies past the elements */
static void
a_cut_off_element(void)
{
	struct hakios_sta_bss bss = network("home", false);
	struct apple80211_scan_result result;

	bss.elements[0] = 1;
	bss.elements[1] = 8;            /* eight rates, of which two are there */
	bss.elements[2] = 0x82;
	bss.elements[3] = 0x84;
	memset(bss.elements + 4, 0x96, 8);      /* (past the elements: not the network's) */
	bss.elements_length = 4;
	heard[heard_n++] = bss;
	memset(&result, FILL, sizeof(result));
	result.elements_length = sizeof(result.elements);
	CHECK(request(false, 11, &result, sizeof(result), NULL) == 0, "given");
	CHECK(result.rate_count == 0, "no rates: %u", result.rate_count);
	CHECK(result.elements_length == 4, "the elements given as heard: %u", result.elements_length);
}

/* the radio's power asked for, on and off; the state in Apple80211's numbers; the access point's address once joined */
static void
power_and_state(void)
{
	struct { uint32_t version, radios, state[4]; } power = { 1, 1, { 1 } };
	static const struct { enum hakios_sta_phase phase; int32_t state; } states[] = {
		{ HAKIOS_STA_IDLE, 0 }, { HAKIOS_STA_SCANNING, 1 }, { HAKIOS_STA_JOINING, 2 }, { HAKIOS_STA_JOINED, 4 },
	};
	struct apple80211req answered;
	uint8_t address[6];

	CHECK(request(true, 19, &power, sizeof(power), NULL) == 0 && power_asked == 1, "on");
	power.state[0] = 0;
	CHECK(request(true, 19, &power, sizeof(power), NULL) == 0 && power_asked == 0, "off");
	power_asked = -1;
	CHECK(request(true, 19, &power, sizeof(power) - 1, NULL) == EINVAL && power_asked == -1, "a request cut short");
	/* the power read back: on while the radio is, off once it is not */
	memset(&power, 0xff, sizeof(power));
	CHECK(request(false, 19, &power, sizeof(power), NULL) == 0 && power.version == 1 && power.radios == 1 && power.state[0] == 1,
	    "read: on (%u)", power.state[0]);
	powered = false;
	CHECK(request(false, 19, &power, sizeof(power), NULL) == 0 && power.state[0] == 0, "read: off (%u)", power.state[0]);
	powered = true;
	for (unsigned i = 0; i < 4; i++) {
		state.phase = states[i].phase;
		CHECK(request(false, 13, NULL, 0, &answered) == 0 && answered.req_val == states[i].state, "phase %u: %d", i,
		    answered.req_val);
	}
	memcpy(state.bss.bssid, "\x02\x11\x22\x33\x44\x55", 6);
	CHECK(request(false, 9, address, sizeof(address), NULL) == 0 && memcmp(address, state.bss.bssid, 6) == 0, "the address");
	state.phase = HAKIOS_STA_JOINING;
	CHECK(request(false, 9, address, sizeof(address), NULL) == ENXIO, "none before it is joined");
}

/* the RSSI bounds wifid's auto-join weighs a known network by (106): wifid's least and most, and the threshold, -80 dBm or
 * as the boot argument says when it is one */
static void
rssi_bounds(void)
{
	struct { uint32_t version; int32_t least, most, threshold; } bounds;
	static const struct { const char *args; int32_t threshold; } cases[] = {
		{ "", -80 }, { "wifi_rssi_threshold=-70", -70 }, { "wifi_rssi_threshold=-127", -127 }, { "wifi_rssi_threshold=5", -80 },
		{ "wifi_rssi_threshold=-128", -80 }, { "wifi_rssi_threshold=0", -80 }, { "wifi_rssi_threshold=-1", -1 },
	};

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		boot_args = cases[i].args;
		memset(&bounds, 0x55, sizeof(bounds));
		CHECK(request(false, 106, &bounds, sizeof(bounds), NULL) == 0 && bounds.version == 1 && bounds.least == -19 &&
		    bounds.most == 65 && bounds.threshold == cases[i].threshold, "\"%s\": %u, %d, %d, %d", cases[i].args, bounds.version,
		    bounds.least, bounds.most, bounds.threshold);
	}
	boot_args = "";
}

/* each channel's DFS and passive marks (CHANNELS_INFO, 207): 5500 MHz is radar's, 5180 passive */
static void
channels_marked(void)
{
	static struct apple80211_channels_info info;

	CHECK(request(false, 207, &info, sizeof(info), NULL) == 0 && info.count == 4, "four: %u", info.count);
	CHECK(info.number[0] == 1 && info.number[2] == 36 && info.number[3] == 100, "numbered: %u %u %u", info.number[0],
	    info.number[2], info.number[3]);
	CHECK(!info.dfs[0] && !info.dfs[1] && !info.dfs[2] && info.dfs[3], "DFS on 5500 alone");
	CHECK(!info.passive[0] && !info.passive[1] && info.passive[2] && !info.passive[3], "passive on 5180 alone");
}

/* what is not built is said to be that, and nothing is asked about a network that is not joined */
static void
refusals(void)
{
	uint8_t buffer[64];

	CHECK(hakios_apple80211_ioctl(0xc0206911, buffer) == EOPNOTSUPP, "another ioctl");
	CHECK(request(false, 999, buffer, sizeof(buffer), NULL) == EOPNOTSUPP, "a get not built");
	CHECK(request(true, 999, buffer, sizeof(buffer), NULL) == EOPNOTSUPP, "a set not built");
	CHECK(request(false, 1, buffer, sizeof(buffer), NULL) == ENXIO, "the name of no network");
	CHECK(request(false, 103, buffer, sizeof(buffer), NULL) == ENXIO, "the network, none");
	char line[96];

	hakios_apple80211_summary();
	snprintf(line, sizeof(line), "get 999 (?) 1 times, answered 0 times; the last refusal: %d (not built)", EOPNOTSUPP);
	/* (on the way back to the loader, interrupts off: to the loader's log, not the console, whose printf would panic there) */
	CHECK(strstr(log_text, line) != NULL && !printed_has(line), "said at the end, to the loader's log: %s", log_text);
}

/* only root changes the radio: another user's sets are refused and change nothing; what it reads it is given */
static void
sets_by_root_alone(void)
{
	static struct apple80211_scan_request scan;
	struct { uint32_t version, radios, state[4]; } power = { 1, 1, { 0 } };
	struct apple80211_scan_result result;
	struct apple80211_assoc assoc;
	struct apple80211req answered;

	memset(&assoc, 0, sizeof(assoc));
	assoc.ssid_length = 4;
	memcpy(assoc.ssid, "cafe", 4);
	memset(&scan, 0, sizeof(scan));
	heard[heard_n++] = network("home", false);
	hakios_apple80211_event(HAKIOS_STA_JOIN_DONE, 0);
	fake_uid = 501;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == EPERM, "a join");
	CHECK(request(true, 19, &power, sizeof(power), NULL) == EPERM, "the power off");
	CHECK(request(true, 22, NULL, 0, NULL) == EPERM, "a leave");
	CHECK(request(true, 10, &scan, sizeof(scan), NULL) == EPERM, "a scan");
	CHECK(joins == 0 && power_asked == -1 && leaves == 0 && scans == 0, "nothing reached the station: %u %d %u %u", joins,
	    power_asked, leaves, scans);
	CHECK(request(false, 21, NULL, 0, &answered) == 0 && answered.req_val == 1, "the join before stays as it went: %d",
	    answered.req_val);
	memset(&power, 0, sizeof(power));
	CHECK(request(false, 19, &power, sizeof(power), NULL) == 0 && power.state[0] == 1, "the power read: on (%u)", power.state[0]);
	memset(&result, FILL, sizeof(result));
	result.elements_length = sizeof(result.elements);
	CHECK(request(false, 11, &result, sizeof(result), NULL) == 0 && memcmp(result.ssid, "home", 4) == 0, "what a scan heard");
	CHECK(request(false, 15, NULL, 0, &answered) == 0 && answered.req_val == 1, "the mode");
	fake_uid = 0;
	power.state[0] = 0;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == 0 && joins == 1, "root's join");
	CHECK(request(true, 19, &power, sizeof(power), NULL) == 0 && power_asked == 0, "root's power off");
	CHECK(request(true, 22, NULL, 0, NULL) == 0 && leaves == 1, "root's leave");
	CHECK(request(true, 10, &scan, sizeof(scan), NULL) == 0 && scans == 1, "root's scan");
}

/* a scan asked for while the last one's networks are being read: the reader reads on, and the scan's end starts it again */
static void
scan_asked_while_read(void)
{
	static struct apple80211_scan_request scan;
	struct apple80211_scan_result result;

	heard[heard_n++] = network("home", false);
	heard[heard_n++] = network("cafe", false);
	memset(&scan, 0, sizeof(scan));
	result.elements_length = sizeof(result.elements);
	CHECK(request(false, 11, &result, sizeof(result), NULL) == 0 && memcmp(result.ssid, "home", 4) == 0, "the first");
	CHECK(request(true, 10, &scan, sizeof(scan), NULL) == 0 && scans == 1, "a scan asked for");
	result.elements_length = sizeof(result.elements);
	CHECK(request(false, 11, &result, sizeof(result), NULL) == 0 && memcmp(result.ssid, "cafe", 4) == 0,
	    "then the second, not the first again: %.4s", (const char *)result.ssid);
	CHECK(request(false, 11, &result, sizeof(result), NULL) == EIO, "and that was all");
	hakios_apple80211_event(HAKIOS_STA_SCAN_DONE, 0);
	result.elements_length = sizeof(result.elements);
	CHECK(request(false, 11, &result, sizeof(result), NULL) == 0 && memcmp(result.ssid, "home", 4) == 0, "over: the first again");
}

/*
 * a join Darwin asked for fails, and the station joins its own network again: what Darwin is told of is its join, failed,
 * and the station's join is no answer of Apple80211's at all
 */
static void
join_not_darwins(void)
{
	struct apple80211_assoc assoc;
	struct apple80211req answered;
	unsigned events;

	memset(&assoc, 0, sizeof(assoc));
	assoc.ssid_length = 4;
	memcpy(assoc.ssid, "cafe", 4);
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == 0, "asked");
	CHECK(request(false, 21, NULL, 0, &answered) == 0 && answered.req_val == 0, "not over yet: %d", answered.req_val);
	join_by_darwin = true;
	hakios_apple80211_event(HAKIOS_STA_JOIN_DONE, EAUTH);
	CHECK(fake_events == 1 && fake_last_event.event_code == EVENT_ASSOC_DONE, "event 9: %u events", fake_events);
	events = fake_events;
	join_by_darwin = false;
	hakios_apple80211_event(HAKIOS_STA_JOIN_DONE, 0);       /* (the station's own network, joined again) */
	CHECK(request(false, 21, NULL, 0, &answered) == 0 && answered.req_val == 21, "failed, a wrong passphrase: %d",
	    answered.req_val);
	CHECK(fake_events == events, "and no event 9 for a join Darwin did not ask for: %u events", fake_events - events);
}

/*
 * a join of a kind the station does not do -- WPA3's SAE, PSK-SHA256, an enterprise network -- is refused at once, the kind
 * named in the log and nothing of the passphrase; asked together with WPA2 and a passphrase it is joined that way
 */
static void
join_kinds_not_built(void)
{
	static const struct { uint16_t upper; const char *said; } refused[] = {
		{ 0x1000, "A JOIN OF WPA3 Personal (SAE) IS REFUSED (authentication 0x1000)" },
		{ 0x0400, "A JOIN OF WPA2 with a passphrase and SHA-256 (PSK-SHA256) IS REFUSED (authentication 0x400)" },
		{ 0x1400, "A JOIN OF WPA3 Personal (SAE) IS REFUSED (authentication 0x1400)" },
		{ 0x0004, "A JOIN OF an enterprise network (802.1X) IS REFUSED (authentication 0x4)" },
		{ 0x0800, "A JOIN OF an enterprise network (802.1X) IS REFUSED (authentication 0x800)" },
	};
	struct apple80211_assoc assoc;
	struct apple80211req answered;

	memset(&assoc, 0, sizeof(assoc));
	assoc.ssid_length = 4;
	memcpy(assoc.ssid, "home", 4);
	assoc.auth_lower = 1;
	assoc.key_cipher = CIPHER_PASSPHRASE;
	assoc.key_length = 12;
	memcpy(assoc.key, "secretpass12", 12);
	assoc.auth_upper = 8;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == 0 && joins == 1, "WPA2 with a passphrase, as before");
	hakios_apple80211_event(HAKIOS_STA_JOIN_DONE, 0);
	for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		assoc.auth_upper = refused[i].upper;
		/* (802.1X brings no key of its own: as such a request came, it was taken for an open network) */
		assoc.key_length = refused[i].upper == 0x0004 || refused[i].upper == 0x0800 ? 0 : 12;
		CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == EINVAL, "%#x refused", refused[i].upper);
		CHECK(strstr(log_text, refused[i].said) != NULL, "and said: %s", refused[i].said);
	}
	CHECK(joins == 1, "none of them reached the station: %u joins", joins);
	CHECK(strstr(log_text, "secretpass12") == NULL && !printed_has("secretpass12") && strstr(log_text, "home") == NULL,
	    "neither the passphrase nor the name in the log");
	CHECK(request(false, 21, NULL, 0, &answered) == 0 && answered.req_val == 1, "the join before stays as it went: %d",
	    answered.req_val);

	/* a network that takes both: Darwin may ask for either, and the station joins it with the passphrase */
	assoc.key_length = 12;
	assoc.auth_upper = 0x1000 | 0x8;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == 0 && joins == 2 && asked_join.security == HAKIOS_STA_WPA2_PSK &&
	    asked_join.passphrase_length == 12, "SAE or WPA2: WPA2");
	assoc.auth_upper = 0x0400 | 0x8;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == 0 && joins == 3, "PSK-SHA256 or WPA2: WPA2");
	/* a kind whose number is not known is left to the station, as before */
	assoc.auth_upper = 0x2000;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == 0 && joins == 4, "a kind not known");

	/* a key of a kind the station does not take (WEP's 104 bits): refused, and said */
	assoc.auth_upper = 0;
	assoc.key_cipher = 2;
	assoc.key_length = 13;
	CHECK(request(true, 20, &assoc, sizeof(assoc), NULL) == EINVAL && joins == 4, "WEP");
	CHECK(strstr(log_text, "A JOIN WITH A KEY OF KIND 2 (13 bytes) IS REFUSED") != NULL, "and said: %s", log_text);
}

/* what the card says it can do: WPA2 among it, and neither protected management frames (52) nor SAE (75), not done */
static void
capabilities(void)
{
	uint8_t bits[16];
	struct apple80211req answered;

	memset(bits, FILL, sizeof(bits));
	CHECK(request(false, 12, bits, sizeof(bits), &answered) == 0, "answered");
	CHECK(untouched(bits, 13, sizeof(bits)), "13 bytes, no more");
	CHECK((bits[3 / 8] & 1u << 3 % 8) != 0, "WPA2 (3)");
	CHECK((bits[52 / 8] & 1u << 52 % 8) == 0 && (bits[75 / 8] & 1u << 75 % 8) == 0, "not 52 (MFP) nor 75 (SAE): %#x %#x",
	    bits[52 / 8], bits[75 / 8]);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "scan_results", scan_results }, { "radio_says_too_much", radio_says_too_much }, { "caller_room", caller_room },
		{ "short_buffer", short_buffer }, { "join", join }, { "scan_request", scan_request }, { "channels_by_number", channels_by_number },
		{ "rates_in_two_elements", rates_in_two_elements }, { "a_cut_off_element", a_cut_off_element }, { "refusals", refusals },
		{ "power_and_state", power_and_state }, { "channels_marked", channels_marked }, { "rssi_bounds", rssi_bounds }, { "link_changes", link_changes }, { "the_most_rates", the_most_rates },
		{ "sets_by_root_alone", sets_by_root_alone }, { "scan_asked_while_read", scan_asked_while_read },
		{ "join_not_darwins", join_not_darwins }, { "join_kinds_not_built", join_kinds_not_built }, { "capabilities", capabilities },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-24s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
