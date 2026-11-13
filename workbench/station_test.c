/*
 * The Wi-Fi station layer (kernel/overlay/bsd/dev/board/sta.c), run as a host program: what Darwin's Wi-Fi side asks
 * -- power, a scan, a join, a leave -- reaches the radio's driver one thing at a time on the layer's own thread, and
 * what comes of it is said back as events. The radio's driver is built outside this tree; here it is a model that
 * notes every call and answers as a scenario says, and that says more than there is room for.
 *
 *   station_test SCENARIO      one scenario a process: the layer keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"
#define KERNEL 1
#include <sys/board/sta.h>

extern char log_text[];

/* ---- the listener: the events, in order ---------------------------------------------------------------- */

static struct { int event, status; } events[64];
static volatile unsigned events_n;
static pthread_mutex_t events_lock = PTHREAD_MUTEX_INITIALIZER;

void
hakios_apple80211_event(enum hakios_sta_event event, int status)
{
	pthread_mutex_lock(&events_lock);
	if (events_n < 64) {
		events[events_n].event = event;
		events[events_n].status = status;
	}
	events_n++;
	pthread_mutex_unlock(&events_lock);
}

static bool
had(int event, int status)
{
	bool found = false;

	pthread_mutex_lock(&events_lock);
	for (unsigned i = 0; i < events_n && i < 64; i++) {
		found |= events[i].event == event && events[i].status == status;
	}
	pthread_mutex_unlock(&events_lock);
	return found;
}

int hakios_wifi_tell(const char *format, ...) { (void)format; return 0; }

/* ---- the radio's driver -------------------------------------------------------------------------------- */

static char calls[512];
static pthread_mutex_t calls_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile enum hakios_sta_phase phase = HAKIOS_STA_IDLE;
static volatile int join_error;
static volatile unsigned networks = 3, radio_channels = 20;
static struct hakios_sta_join joined_with;

static void
call(const char *what)
{
	pthread_mutex_lock(&calls_lock);
	strncat(calls, what, sizeof(calls) - strlen(calls) - 1);
	pthread_mutex_unlock(&calls_lock);
}

static int radio_power(bool on) { call(on ? "on " : "off "); phase = on ? HAKIOS_STA_IDLE : HAKIOS_STA_OFF; return 0; }
static int radio_scan(const struct hakios_sta_scan *what) { (void)what; call("scan "); return 0; }
static int radio_leave(void) { call("leave "); phase = HAKIOS_STA_IDLE; return 0; }
static void radio_state(struct hakios_sta_state *state) { state->phase = phase; }

static int
radio_join(const struct hakios_sta_join *what)
{
	char line[64];

	snprintf(line, sizeof(line), "join:%.*s ", what->ssid_length, (const char *)what->ssid);
	call(line);
	joined_with = *what;
	if (join_error == 0) {
		phase = HAKIOS_STA_JOINED;
	}
	return join_error;
}

static void
radio_heard(void (*each)(const struct hakios_sta_bss *bss, void *context), void *context)
{
	static struct hakios_sta_bss bss;

	for (unsigned i = 0; i < networks; i++) {
		memset(&bss, 0, sizeof(bss));
		bss.ssid_length = (uint8_t)snprintf((char *)bss.ssid, sizeof(bss.ssid), "net%u", i);
		bss.frequency = 2412;
		each(&bss, context);
	}
}

static unsigned
radio_channels_of(struct hakios_sta_channel *out, unsigned most)
{
	for (unsigned i = 0; i < radio_channels && i < most; i++) {
		out[i].frequency = (uint16_t)(5000 + 5 * i);
	}
	return radio_channels;          /* (how many it has: more, some day, than it was given room for) */
}

static const struct hakios_sta_ops radio = {
	.power = radio_power, .scan = radio_scan, .heard = radio_heard, .join = radio_join, .leave = radio_leave,
	.state = radio_state, .channels = radio_channels_of, .address = { 2, 0, 0, 0, 0, 1 },
};

#include "board/sta.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

static bool
called(const char *what)
{
	bool found;

	pthread_mutex_lock(&calls_lock);
	found = strstr(calls, what) != NULL;
	pthread_mutex_unlock(&calls_lock);
	return found;
}

static struct hakios_sta_join
network(const char *name)
{
	struct hakios_sta_join join = { .security = HAKIOS_STA_WPA2_PSK };

	join.ssid_length = (uint8_t)strlen(name);
	memcpy(join.ssid, name, join.ssid_length);
	memcpy(join.passphrase, "a passphrase", 12);
	join.passphrase_length = 12;
	return join;
}

/* ---- scenarios -------------------------------------------------------------------------------------- */

/* nobody asks of a radio that is not there; once it is, a scan's networks can be read, one by one */
static void
scan(void)
{
	struct hakios_sta_scan what = { 0 };
	struct hakios_sta_bss bss;

	CHECK(!hakios_sta_present() && hakios_sta_ask_scan(&what) != 0, "no radio yet: refused");
	hakios_sta_attach(&radio);
	CHECK(hakios_sta_present() && had(HAKIOS_STA_POWER, 1), "the radio is there, and on");
	CHECK(hakios_sta_ask_scan(&what) == 0, "a scan asked for");
	WAIT_FOR(had(HAKIOS_STA_SCAN_DONE, 0), 3000);
	CHECK(had(HAKIOS_STA_SCAN_DONE, 0) && called("scan "), "done: %s", calls);
	CHECK(hakios_sta_heard(2, &bss) && memcmp(bss.ssid, "net2", 4) == 0 && !hakios_sta_heard(3, &bss), "three networks, by index");
}

/* a radio that hears more than the list has room for: the list keeps what fits */
static void
too_many_heard(void)
{
	struct hakios_sta_scan what = { 0 };
	struct hakios_sta_bss bss;

	networks = 300;
	hakios_sta_attach(&radio);
	CHECK(hakios_sta_ask_scan(&what) == 0, "asked");
	WAIT_FOR(had(HAKIOS_STA_SCAN_DONE, 0), 3000);
	CHECK(hakios_sta_heard(127, &bss) && !hakios_sta_heard(128, &bss), "128 kept, and no more");
}

/* a radio that says it has more channels than it was given room for: the count is held to the room */
static void
too_many_channels(void)
{
	struct hakios_sta_channel out[200];

	radio_channels = 150;
	hakios_sta_attach(&radio);
	CHECK(hakios_sta_channels(out, 200) == 96, "96 channels, the room there is: %u", hakios_sta_channels(out, 200));
	CHECK(out[95].frequency == 5000 + 5 * 95, "the last of them as the radio gave it");
}

/* a join: done, then the link; a leave: the link gone */
static void
join_and_leave(void)
{
	struct hakios_sta_join home = network("home");

	hakios_sta_attach(&radio);
	CHECK(hakios_sta_ask_join(&home) == 0, "asked");
	WAIT_FOR(had(HAKIOS_STA_LINK, 1), 3000);
	CHECK(had(HAKIOS_STA_JOIN_DONE, 0) && had(HAKIOS_STA_LINK, 1) && called("join:home "), "joined: %s", calls);
	CHECK(hakios_sta_ask_leave() == 0, "a leave asked for");
	WAIT_FOR(called("leave "), 3000);
	CHECK(called("leave ") && had(HAKIOS_STA_LINK, 0), "left: %s", calls);
}

/* the standing network is joined again whenever the radio comes on */
static void
standing_network(void)
{
	struct hakios_sta_join home = network("home");

	hakios_sta_attach(&radio);
	CHECK(hakios_sta_ask_join_standing(&home) == 0, "the standing network asked for");
	WAIT_FOR(had(HAKIOS_STA_LINK, 1), 3000);
	CHECK(hakios_sta_ask_power(false) == 0, "off");
	WAIT_FOR(called("off "), 3000);
	CHECK(hakios_sta_ask_power(true) == 0, "on");
	WAIT_FOR(called("off on join:home "), 3000);
	CHECK(strstr(calls, "off on join:home ") != NULL, "off, on, and joined again: %s", calls);
}

/* a join that does not come through (a wrong passphrase) leaves the standing network as it was, and joins it again */
static void
wrong_passphrase(void)
{
	struct hakios_sta_join home = network("home"), other = network("other");

	hakios_sta_attach(&radio);
	CHECK(hakios_sta_ask_join_standing(&home) == 0, "home, standing");
	WAIT_FOR(had(HAKIOS_STA_LINK, 1), 3000);
	join_error = EACCES;
	CHECK(hakios_sta_ask_join_standing(&other) == 0, "another network, whose passphrase is wrong");
	WAIT_FOR(had(HAKIOS_STA_JOIN_DONE, EACCES), 3000);
	join_error = 0;
	WAIT_FOR(called("join:other join:home "), 3000);
	CHECK(strstr(calls, "join:home join:other join:home ") != NULL, "home again after the failed join: %s", calls);
}

/* a network lost by itself is said as such */
static void
lost(void)
{
	hakios_sta_attach(&radio);
	hakios_sta_lost(0);
	CHECK(had(HAKIOS_STA_LINK, ECONNRESET), "lost: a reset connection");
}

/* the radio going away waits for a call under way, and nothing is asked of it after */
static void
radio_goes(void)
{
	struct hakios_sta_scan what = { 0 };

	hakios_sta_attach(&radio);
	hakios_sta_attach(NULL);
	CHECK(!hakios_sta_present() && hakios_sta_ask_scan(&what) != 0, "gone: refused");
	usleep(100 * 1000);
	CHECK(!called("scan "), "and nothing reached it: %s", calls);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "scan", scan }, { "too_many_heard", too_many_heard }, { "too_many_channels", too_many_channels },
		{ "join_and_leave", join_and_leave }, { "standing_network", standing_network }, { "wrong_passphrase", wrong_passphrase },
		{ "lost", lost }, { "radio_goes", radio_goes },
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
			fprintf(stderr, "%-18s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
