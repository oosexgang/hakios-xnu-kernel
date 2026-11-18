/*
 * The Wi-Fi station layer (drivers/board/sta.c), run as a host program: what Darwin's Wi-Fi side asks
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
static volatile unsigned join_takes_ms;          /* a join that takes a while */
static volatile unsigned networks = 3, radio_channels = 20;
static struct hakios_sta_join joined_with;

static void
call(const char *what)
{
	pthread_mutex_lock(&calls_lock);
	strncat(calls, what, sizeof(calls) - strlen(calls) - 1);
	pthread_mutex_unlock(&calls_lock);
}

static volatile int power_error;
static volatile unsigned scan_takes_ms, state_takes_ms;        /* a call that takes a while */
static volatile bool scan_returned, state_returned;

static int
radio_power(bool on)
{
	call(on ? "on " : "off ");
	if (power_error == 0) {
		phase = on ? HAKIOS_STA_IDLE : HAKIOS_STA_OFF;
	}
	return power_error;
}

static int
radio_scan(const struct hakios_sta_scan *what)
{
	(void)what;
	call("scan ");
	usleep(scan_takes_ms * 1000);
	scan_returned = true;
	return 0;
}

static int radio_leave(void) { call("leave "); phase = HAKIOS_STA_IDLE; return 0; }

static void
radio_state(struct hakios_sta_state *state)
{
	usleep(state_takes_ms * 1000);
	state->phase = phase;
	state_returned = true;
}

static int
radio_join(const struct hakios_sta_join *what)
{
	char line[64];

	snprintf(line, sizeof(line), "join:%.*s ", what->ssid_length, (const char *)what->ssid);
	call(line);
	usleep(join_takes_ms * 1000);
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

/*
 * The layer's wakeup of its thread, which a scenario can have wait, once, on the asker's side, for the join it woke the
 * thread for to be over and the next begun: what the layer does with its lock let go of before the wakeup, a busy
 * machine may do too.
 */
static volatile bool hold_after_wakeup;
static bool called(const char *what);

static void
wakeup_then_hold(void *event)
{
	thread_wakeup(event);
	if (hold_after_wakeup) {
		hold_after_wakeup = false;
		for (int waited = 0; !called("join:other join:home ") && waited < 3000; waited += 5) {
			usleep(5000);
		}
	}
}

#define thread_wakeup(event) wakeup_then_hold(event)
#include "board/sta.c"
#undef thread_wakeup

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

/* the last event said, or -1 */
static int
last_event(int *status)
{
	int event = -1;

	pthread_mutex_lock(&events_lock);
	if (events_n > 0 && events_n <= 64) {
		event = events[events_n - 1].event;
		*status = events[events_n - 1].status;
	}
	pthread_mutex_unlock(&events_lock);
	return event;
}

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
	/* (the link's going is told after the leave is called: waited for too, or a busy machine sees the one without the other) */
	WAIT_FOR(called("leave ") && had(HAKIOS_STA_LINK, 0), 3000);
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

/* the standing network lost by itself is joined again, 2 s later; while that fails, again 2 s after each try */
static void
lost_and_joined_again(void)
{
	struct hakios_sta_join home = network("home");

	hakios_sta_attach(&radio);
	CHECK(hakios_sta_ask_join_standing(&home) == 0, "home, standing");
	WAIT_FOR(had(HAKIOS_STA_LINK, 1), 3000);
	join_error = EIO;
	hakios_sta_lost(0);
	usleep(1000 * 1000);
	CHECK(strcmp(calls, "join:home ") == 0, "not at once: %s", calls);
	WAIT_FOR(called("join:home join:home join:home "), 10000);
	CHECK(strcmp(calls, "join:home join:home join:home ") == 0, "after 2 s, and after 2 s more when that failed: %s", calls);
	join_error = 0;
	/* (2 s after the last failed try; room for a busy computer) */
	WAIT_FOR(called("join:home join:home join:home join:home "), 10000);
	CHECK(had(HAKIOS_STA_JOIN_DONE, 0) && strcmp(calls, "join:home join:home join:home join:home ") == 0, "joined: %s", calls);
}

/* a network that was asked for once, not the standing one, is not joined again by itself */
static void
lost_without_a_standing_network(void)
{
	struct hakios_sta_join cafe = network("cafe");

	hakios_sta_attach(&radio);
	CHECK(hakios_sta_ask_join(&cafe) == 0, "asked");
	WAIT_FOR(had(HAKIOS_STA_LINK, 1), 3000);
	hakios_sta_lost(0);
	usleep(2600 * 1000);
	CHECK(strcmp(calls, "join:cafe ") == 0, "nothing more: %s", calls);
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

/* the radio switched off while joined: the link said gone first, then the power; off while not joined: the power alone */
static void
off_while_joined(void)
{
	struct hakios_sta_join home = network("home");
	struct hakios_sta_state state;
	int status = 0;

	hakios_sta_attach(&radio);
	CHECK(hakios_sta_ask_join(&home) == 0, "asked");
	WAIT_FOR(had(HAKIOS_STA_LINK, 1), 3000);
	hakios_sta_state(&state);
	CHECK(state.phase == HAKIOS_STA_JOINED, "joined, as the radio says: %d", state.phase);
	CHECK(hakios_sta_ask_power(false) == 0 && !hakios_sta_powered(), "off asked for");
	WAIT_FOR(had(HAKIOS_STA_POWER, 0), 3000);
	CHECK(had(HAKIOS_STA_LINK, 0), "the link gone");
	CHECK(last_event(&status) == HAKIOS_STA_POWER && status == 0, "and then the power: %d %d", last_event(&status), status);
	CHECK(hakios_sta_ask_power(true) == 0 && hakios_sta_powered(), "on again");
	WAIT_FOR(called("off on "), 3000);
	events_n = 0;
	CHECK(hakios_sta_ask_power(false) == 0, "and off, not joined");
	WAIT_FOR(had(HAKIOS_STA_POWER, 0), 3000);
	CHECK(!had(HAKIOS_STA_LINK, 0), "no link to say gone");
}

/* the radio's driver refuses to switch: the power is said as it stays */
static void
power_refused(void)
{
	int status = 9;

	hakios_sta_attach(&radio);
	power_error = EIO;
	events_n = 0;
	CHECK(hakios_sta_ask_power(false) == 0, "off asked for");
	WAIT_FOR(events_n > 0, 3000);
	CHECK(last_event(&status) == HAKIOS_STA_POWER && status == 1, "still on: %d", status);
	CHECK(hakios_sta_powered(), "and said to be on, as it is: not off, as was asked");
}

/* another standing network that does come through: it is the standing one now, and the one before is not joined again */
static void
another_standing_network(void)
{
	struct hakios_sta_join home = network("home"), other = network("other");

	hakios_sta_attach(&radio);
	CHECK(hakios_sta_ask_join_standing(&home) == 0, "home, standing");
	WAIT_FOR(called("join:home "), 3000);
	CHECK(hakios_sta_ask_join_standing(&other) == 0, "then other");
	WAIT_FOR(called("join:other "), 3000);
	usleep(100 * 1000);
	CHECK(strcmp(calls, "join:home join:other ") == 0, "home is not joined again: %s", calls);
	CHECK(hakios_sta_ask_power(false) == 0 && hakios_sta_ask_power(true) == 0, "off, on");
	WAIT_FOR(called("on join:"), 3000);
	CHECK(strstr(calls, "on join:other ") != NULL, "and other is the one joined: %s", calls);
}

static void *
read_state(void *state)
{
	hakios_sta_state(state);
	return NULL;
}

/* the radio going away while a scan is under way, and while its state is being read: both are waited for */
static void
goes_during_a_call(void)
{
	struct hakios_sta_scan what = { 0 };
	struct hakios_sta_state state;
	pthread_t reader;

	hakios_sta_attach(&radio);
	WAIT_FOR(!hakios_sta_busy(), 3000);
	CHECK(!hakios_sta_busy(), "nothing asked: not busy");
	scan_takes_ms = 300;
	CHECK(hakios_sta_ask_scan(&what) == 0, "a long scan");
	WAIT_FOR(called("scan "), 3000);
	CHECK(hakios_sta_busy(), "busy while it runs");
	hakios_sta_attach(NULL);
	CHECK(scan_returned, "the radio's going waited for the scan");
	CHECK(!hakios_sta_busy() && !hakios_sta_powered(), "gone: neither busy nor on");
	hakios_sta_attach(&radio);
	state_takes_ms = 300;
	pthread_create(&reader, NULL, read_state, &state);
	usleep(100 * 1000);
	hakios_sta_attach(NULL);
	CHECK(state_returned, "... and for its state being read");
	pthread_join(reader, NULL);
}

/* the radio's address, once there is a radio */
static void
address(void)
{
	uint8_t mac[6] = { 0 };

	CHECK(!hakios_sta_address(mac) && !hakios_sta_powered(), "no radio: no address");
	hakios_sta_attach(&radio);
	CHECK(hakios_sta_address(mac) && mac[0] == 2 && mac[5] == 1, "the radio's: %02x..%02x", mac[0], mac[5]);
}

/* a rejoin under way when the radio goes, which fails: the next radio is not asked for it, nor for anything else */
static void
rejoin_survives_detach(void)
{
	struct hakios_sta_join home = network("home");

	hakios_sta_attach(&radio);
	CHECK(hakios_sta_ask_join_standing(&home) == 0, "home, standing");
	WAIT_FOR(had(HAKIOS_STA_LINK, 1), 3000);
	join_error = EIO;
	join_takes_ms = 500;
	hakios_sta_lost(0);
	WAIT_FOR(called("join:home join:home "), 4000);         /* the rejoin, under way */
	CHECK(called("join:home join:home "), "the rejoin begun: %s", calls);
	hakios_sta_attach(NULL);                                 /* the radio goes while it runs, and it fails */
	join_error = 0;
	join_takes_ms = 0;
	pthread_mutex_lock(&calls_lock);
	calls[0] = 0;
	pthread_mutex_unlock(&calls_lock);
	hakios_sta_attach(&radio);                               /* a new radio, which nobody asks for anything */
	usleep(3000 * 1000);
	CHECK(!called("join:"), "the new radio is asked for nothing: %s", calls);
}

/*
 * another standing network whose join fails at once, before the asker is back: the one before is the standing network
 * still, and the one that failed is not joined when the radio comes on
 */
static void
standing_set_with_its_join(void)
{
	struct hakios_sta_join home = network("home"), other = network("other");

	hakios_sta_attach(&radio);
	CHECK(hakios_sta_ask_join_standing(&home) == 0, "home, standing");
	WAIT_FOR(had(HAKIOS_STA_LINK, 1), 3000);
	join_error = EACCES;
	hold_after_wakeup = true;
	CHECK(hakios_sta_ask_join_standing(&other) == 0, "other, whose passphrase is wrong");
	CHECK(called("join:home join:other join:home "), "other failed, and home was asked for again: %s", calls);
	join_error = 0;
	WAIT_FOR(!hakios_sta_busy(), 3000);
	CHECK(hakios_sta_ask_power(false) == 0 && hakios_sta_ask_power(true) == 0, "off, on");
	WAIT_FOR(called("on join:"), 3000);
	CHECK(strstr(calls, "on join:home ") != NULL, "home is the one joined: %s", calls);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "scan", scan }, { "too_many_heard", too_many_heard }, { "too_many_channels", too_many_channels },
		{ "join_and_leave", join_and_leave }, { "standing_network", standing_network }, { "wrong_passphrase", wrong_passphrase },
		{ "lost", lost }, { "lost_and_joined_again", lost_and_joined_again },
		{ "lost_without_a_standing_network", lost_without_a_standing_network }, { "radio_goes", radio_goes }, { "off_while_joined", off_while_joined }, { "power_refused", power_refused },
		{ "another_standing_network", another_standing_network }, { "goes_during_a_call", goes_during_a_call }, { "address", address },
		{ "rejoin_survives_detach", rejoin_survives_detach }, { "standing_set_with_its_join", standing_set_with_its_join },
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
