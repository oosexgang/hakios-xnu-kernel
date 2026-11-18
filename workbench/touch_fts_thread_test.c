/*
 * The FT3658U driver's thread (drivers/board/fts.c) run as it is, against a model of the controller behind its SPI
 * transfer, of the registers of the pins and the clock controller (they keep what is written), and of RPMh and its
 * command database (each request noted). What is tested: a finger whose reports stop is lifted after a quarter of a
 * second, however slowly the reads fail; the resets counted against the controller are forgotten after a minute without
 * a failure, an idle one included, and four troubles in a row still end the polling; when bring-up fails, when the
 * analog supply is refused and when the polling gives up, the controller is powered off -- in reset, the analog then
 * the I/O supply's vote withdrawn, only the clock votes the driver added dropped, the pins as found; the same at the
 * run's end (hakios_fts_quiesce()), with no more polling, also when the thread was stopped in the middle of a transfer
 * through the serial engine or by hand; the panel's size is the board's coordinates, the first phone's without; and
 * the transfer by hand never drives the chip's data output.
 *
 *   touch_fts_thread_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

/* the test's own clock; the driver's runs `skew' ahead of it, which a scenario moves to let a minute pass */
static uint64_t real_ns(void) { return mach_absolute_time(); }
static volatile uint64_t skew;
#define mach_absolute_time()    (mach_absolute_time() + skew)

static struct { uint32_t type, finger, x, y; uint64_t t; } posted[256];
static volatile unsigned posts;
void
hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	if (posts < 256) {
		posted[posts].type = type;
		posted[posts].finger = finger;
		posted[posts].x = x;
		posted[posts].y = y;
		posted[posts].t = real_ns();
	}
	posts++;
}

/* ---- what happened, in order: the pins', the votes', RPMh's and the engine's ----------------------------------------- */

enum { EV_CFG, EV_IO, EV_VOTE, EV_RPMH, EV_RECOVER };
static struct { int kind; uint32_t a, b; } ev[1 << 14];
static unsigned evs;
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;

static void
note(int kind, uint32_t a, uint32_t b)
{
	pthread_mutex_lock(&ev_lock);
	if (evs < sizeof(ev) / sizeof(ev[0])) {
		ev[evs].kind = kind;
		ev[evs].a = a;
		ev[evs].b = b;
		evs++;
	}
	pthread_mutex_unlock(&ev_lock);
}

/* the last event of a kind (and its a, b where they are not ~0) at or after `from'; -1: none */
static int
last_event(int kind, uint32_t a, uint32_t b, int from)
{
	int found = -1;

	pthread_mutex_lock(&ev_lock);
	for (unsigned i = from < 0 ? 0 : (unsigned)from; i < evs; i++) {
		if (ev[i].kind == kind && (a == ~0u || ev[i].a == a) && (b == ~0u || ev[i].b == b)) {
			found = (int)i;
		}
	}
	pthread_mutex_unlock(&ev_lock);
	return found;
}

/* ---- the registers: sparse, keeping what is written ------------------------------------------------------------- */

#define SLOTS 8192
static vm_offset_t ka[SLOTS];
static uint32_t kv[SLOTS];
static pthread_mutex_t mm = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *
slot(vm_offset_t a)
{
	unsigned h = (unsigned)(a >> 2) % SLOTS;

	while (ka[h] && ka[h] != a) {
		h = (h + 1) % SLOTS;
	}
	ka[h] = a;
	return &kv[h];
}

/* the board's wiring, as the test reads it (filled in by phone()) */
static vm_offset_t tlmm_base, vote_address, cmd_address;
enum { P_DATA_IN, P_DATA_OUT, P_CLK, P_CS, P_INT, P_RESET, PIN_COUNT };
static unsigned pin[PIN_COUNT];
static bool stall_on_select;                    /* the next chip select taken low by hand stops the thread there */
static volatile bool stalled;
static volatile unsigned miso_driven;           /* the chip's data output configured as an output */

static int
which_pin(vm_offset_t a, unsigned *reg)
{
	if (tlmm_base == 0 || a < tlmm_base || a >= tlmm_base + 0x100000) {
		return -1;
	}
	*reg = (unsigned)((a - tlmm_base) % 0x1000);
	for (int i = 0; i < PIN_COUNT; i++) {
		if (pin[i] == (a - tlmm_base) / 0x1000) {
			return i;
		}
	}
	return -1;
}

uint32_t
hakios_test_rd(vm_offset_t a)
{
	pthread_mutex_lock(&mm);
	uint32_t v = *slot(a);
	pthread_mutex_unlock(&mm);
	return v;
}

void
hakios_test_wr(vm_offset_t a, uint32_t v)
{
	unsigned reg = 0;
	int p = which_pin(a, &reg);

	if (a == cmd_address) {
		v &= ~1u;                       /* (the engine's clock takes its configuration at once) */
	}
	if (p >= 0 && reg == 0) {
		note(EV_CFG, pin[p], v);
		if (p == P_DATA_IN && (v & (1u << 9))) {
			miso_driven++;
		}
	}
	if (p == P_CS && reg == 4) {
		note(EV_IO, pin[p], v);
		if (v == 0 && stall_on_select) {
			stalled = true;
			for (;;) {
				usleep(100000);     /* (a core stopped with the chip selected) */
			}
		}
	}
	if (p == P_RESET && reg == 4) {
		note(EV_IO, pin[p], v);
	}
	if (a == vote_address) {
		note(EV_VOTE, 0, v);
	}
	pthread_mutex_lock(&mm);
	*slot(a) = v;
	pthread_mutex_unlock(&mm);
}

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
vm_offset_t ml_io_map_wcomb(vm_offset_t phys, vm_size_t size __unused) { return phys; }
void hakios_tl(const char *format __unused, ...) { }
static volatile bool in_quiesce;
static volatile unsigned locks, locks_in_quiesce;
void hakios_rpmh_lock(void) { locks++; locks_in_quiesce += in_quiesce; }
void hakios_rpmh_unlock(void) { }

/* ---- RPMh and its command database, in place of board/cmd_db.h and board/rpmh.h ---------------------------------- */

#define HAKIOS_CMD_DB_H
struct cmd_db_header { uint8_t magic[4]; };
static bool hakios_cmd_db_valid(const struct cmd_db_header *db __unused) { return true; }
/* a regulator's address by its name (the two phones' four); RPMh's regulators are at 0x4xxxx */
static uint32_t
supply_base(const char *name)
{
	static const char *const names[] = { "ldoc2", "ldoc7", "ldob5", "ldob9" };

	for (unsigned i = 0; i < 4; i++) {
		if (strcmp(name, names[i]) == 0) {
			return 0x40100 + 0x100 * i;
		}
	}
	return 0x40f00;
}
static uint32_t hakios_cmd_db_address(const struct cmd_db_header *db __unused, uint32_t s __unused, const char *n) { return supply_base(n); }

#define HAKIOS_RPMH_H
#include "board/time.h"
static uint32_t refused_supply;                 /* a supply whose switching on RPMh does not answer */
static const char *
hakios_rpmh_send_rsc(vm_offset_t r __unused, uint32_t o __unused, const uint32_t *a, const uint32_t *d, unsigned c)
{
	for (unsigned i = 0; i < c; i++) {
		note(EV_RPMH, a[i], d[i]);
	}
	if (refused_supply != 0 && c == 3 && a[0] == refused_supply && d[1] == 1) {
		return "no answer to the request";
	}
	return NULL;
}

/* ---- the serial engine, in place of board/geni_spi.h: its transfer is the controller's model ------------------------- */

#define HAKIOS_GENI_SPI_H
#define GENI_SER_M_CLK_CFG      (se + 0x048)
#define GENI_IF_DISABLE_RO      (se + 0x064)
#define GENI_FW_REVISION_RO     (se + 0x068)
#define SE_HW_PARAM_0           (se + 0xe24)
static const char *geni_problem;                /* why the engine cannot be used (NULL: it can) */
static uint32_t rcg_rate(uint32_t a __unused, uint32_t b __unused, uint32_t c __unused) { return 0; }
static const char *geni_spi_init(uint32_t hz __unused) { return geni_problem; }
static bool model_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length);
static bool geni_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length) { return model_transfer(tx, rx, length); }
static volatile unsigned recovered;
static void __attribute__((unused)) geni_recover(void) { recovered++; note(EV_RECOVER, 0, 0); }

/* the run's end, weak: a driver without one is shown left running rather than failing to build */
void hakios_fts_quiesce(void) __attribute__((weak));

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "board/fts.c"
#undef printf

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited_ = 0; !(cond) && waited_ < (ms); waited_ += 5) usleep(5000); } while (0)

/* ---- the controller: its identity, a report, and the ways its reports go wrong --------------------------------------- */

static enum {
	ANSWERS,                                /* every read answered */
	REPORTS_FAIL,                           /* a report's read fails at once (the identity is still read) */
	REPORTS_FAIL_SLOWLY,                    /* ... after 20 ms */
	REPORT_STALLS,                          /* the next report's read never ends (a core stopped in it) */
	ANOTHER_ID,                             /* another chip's identity */
} chip;
static volatile unsigned failing_reads;         /* so many transfers of a report fail, whatever `chip' says */
static uint8_t frame[FTS_REPORT_BYTES];
static volatile unsigned transfers, reports_read;

static bool
model_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	uint32_t n = length - FTS_READ_PREFIX - 2;
	uint8_t reg = tx[0];

	transfers++;
	if (reg == 0x01) {
		if (chip == REPORT_STALLS) {
			stalled = true;
			for (;;) {
				usleep(100000);
			}
		}
		if (failing_reads > 0) {
			failing_reads--;
			return false;
		}
		if (chip == REPORTS_FAIL_SLOWLY) {
			usleep(20000);
			return false;
		}
		if (chip == REPORTS_FAIL) {
			return false;
		}
	}
	memset(rx, 0, length);
	for (uint32_t i = 0; i < n; i++) {
		rx[FTS_READ_PREFIX + i] = reg == 0x01 ? frame[i] : reg == 0xa3 ? (chip == ANOTHER_ID ? 0x57 : 0x56) :
		    reg == 0x9f ? 0x52 : reg == 0x00 ? 0x00 : 0x12;
	}
	uint16_t crc = fts_crc(rx + FTS_READ_PREFIX, n);
	rx[FTS_READ_PREFIX + n] = (uint8_t)crc;
	rx[FTS_READ_PREFIX + n + 1] = (uint8_t)(crc >> 8);
	if (reg == 0x01) {
		reports_read++;
	}
	return true;
}

/* the report of no finger (all 0xff), or of one finger at x, y */
static void
no_finger(void)
{
	memset(frame, 0xff, sizeof(frame));
}

static void
finger(unsigned x, unsigned y)
{
	memset(frame, 0xff, sizeof(frame));
	frame[0] = 0;
	frame[1] = 1;
	frame[2] = (uint8_t)(2u << 6 | (x >> 8));
	frame[3] = (uint8_t)x;
	frame[4] = (uint8_t)(0u << 4 | (y >> 8));
	frame[5] = (uint8_t)y;
	frame[6] = frame[7] = 0;
}

static unsigned
times_logged(const char *text)
{
	unsigned n = 0;

	for (const char *at = log_text; (at = strstr(at, text)) != NULL; at++) {
		n++;
	}
	return n;
}

static int
posted_of(uint32_t type)
{
	for (unsigned i = 0; i < posts && i < 256; i++) {
		if (posted[i].type == type) {
			return (int)i;
		}
	}
	return -1;
}

/* ---- a phone, its pins and votes as the firmware left them, and the driver started on it -------------------------------- */

static uint32_t cfg_found[PIN_COUNT], io_found[PIN_COUNT], votes_found;

static void
phone(const char *name)
{
	const struct hakios_board *b;

	board = name;
	boot_args = "touchscreen=1";
	b = hakios_board();
	tlmm_base = b->pins.tlmm;
	vote_address = b->clocks.gcc + b->touch.vote_register;
	cmd_address = b->clocks.gcc + b->touch.se_clock + 4;
	pin[P_DATA_IN] = b->touch.data_in;
	pin[P_DATA_OUT] = b->touch.data_out;
	pin[P_CLK] = b->touch.clock;
	pin[P_CS] = b->touch.chip_select;
	pin[P_INT] = b->touch.interrupt;
	pin[P_RESET] = b->touch.reset;
	for (int i = 0; i < PIN_COUNT; i++) {
		/* (the bus's pins the engine's, pulled down; each a value of its own; every other one driving high) */
		cfg_found[i] = (i < P_INT ? 1u << 2 : 0) | 1u | (unsigned)i << 6;
		io_found[i] = i & 1 ? 2 : 0;
		pthread_mutex_lock(&mm);
		*slot(tlmm_base + 0x1000ul * pin[i]) = cfg_found[i];
		*slot(tlmm_base + 0x1000ul * pin[i] + 4) = io_found[i];
		pthread_mutex_unlock(&mm);
	}
	/* the wrapper's two core clocks voted for already (another engine of it), and a vote of somebody else's */
	votes_found = 1u << b->touch.vote[0] | 1u << b->touch.vote[1] | 1u << 31;
	pthread_mutex_lock(&mm);
	*slot(vote_address) = votes_found;
	pthread_mutex_unlock(&mm);
	no_finger();
}

static void
start(void)
{
	hakios_fts_init();
	WAIT_FOR(logged("touchscreen ready") || logged("touchscreen unavailable"), 10000);
}

static void
started(void)
{
	start();
	CHECK(logged("touchscreen ready"), "the controller brought up: %s", log_text);
}

/*
 * Powered off: in reset, the analog supply's vote withdrawn and then the I/O supply's, the votes the driver added
 * dropped and the others left, the pins configured as found (the reset pin low under its configuration)
 */
static void
powered_off(const char *when)
{
	const struct hakios_board *b = hakios_board();
	uint32_t io = supply_base(b->touch.io_supply) + VRM_ENABLE, analog = supply_base(b->touch.analog_supply) + VRM_ENABLE;
	int io_off = last_event(EV_RPMH, io, 0, 0), io_on = last_event(EV_RPMH, io, 1, 0);
	int analog_off = last_event(EV_RPMH, analog, 0, 0), analog_on = last_event(EV_RPMH, analog, 1, 0);

	CHECK(io_off > io_on && analog_off > analog_on, "%s: both supplies' votes withdrawn (I/O on %d off %d, analog on %d off %d)",
	    when, io_on, io_off, analog_on, analog_off);
	CHECK(analog_off < io_off, "%s: the analog supply's first (%d, %d)", when, analog_off, io_off);
	CHECK(!(hakios_test_rd(tlmm_base + 0x1000ul * pin[P_RESET] + 4) & 2), "%s: the controller held in reset", when);
	CHECK(hakios_test_rd(vote_address) == votes_found, "%s: only the votes the driver added dropped: %#x, found %#x", when,
	    hakios_test_rd(vote_address), votes_found);
	for (int i = 0; i < PIN_COUNT; i++) {
		uint32_t cfg = hakios_test_rd(tlmm_base + 0x1000ul * pin[i]), level = hakios_test_rd(tlmm_base + 0x1000ul * pin[i] + 4);

		CHECK(cfg == cfg_found[i], "%s: gpio%u configured as found: %#x, found %#x", when, pin[i], cfg, cfg_found[i]);
		CHECK(i == P_RESET || level == io_found[i], "%s: gpio%u's level as found: %#x, found %#x", when, pin[i], level,
		    io_found[i]);
	}
}

/* the polling has stopped: no transfer for a while */
static void
no_more_polling(const char *when)
{
	unsigned before = transfers;

	usleep(300 * 1000);
	CHECK(transfers == before, "%s: no more transfers (%u in 300 ms)", when, transfers - before);
}

/* the run's end, timed; false (and why) where the driver has none */
static bool
run_end(uint64_t *took_ms)
{
	uint64_t t0 = real_ns();
	void (*volatile quiesce)(void) = hakios_fts_quiesce;

	if (quiesce == NULL) {
		unsigned before = transfers;

		usleep(300 * 1000);
		CHECK(quiesce != NULL, "the driver has no hakios_fts_quiesce(): nothing stops it at the run's end -- "
		    "%u transfers in the next 300 ms, reset %s, %s", transfers - before,
		    hakios_test_rd(tlmm_base + 0x1000ul * pin[P_RESET] + 4) & 2 ? "high" : "low",
		    last_event(EV_RPMH, ~0u, 0, 0) < 0 ? "no supply's vote withdrawn" : "a supply's vote withdrawn");
		return false;
	}
	in_quiesce = true;
	quiesce();
	in_quiesce = false;
	*took_ms = (real_ns() - t0) / 1000000;
	return true;
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* the run's end while the controller is polled: polling stopped, the controller powered off, RPMh asked without its lock */
static void
run_end_powers_off(void)
{
	uint64_t took = 0;

	phone("phone-cmd");
	started();
	WAIT_FOR(reports_read >= 5, 2000);
	if (!run_end(&took)) {
		return;
	}
	CHECK(took < 500, "bounded: %llu ms", (unsigned long long)took);
	no_more_polling("at the run's end");
	powered_off("at the run's end");
	CHECK(locks_in_quiesce == 0, "RPMh's lock not taken on the way out (%u times)", locks_in_quiesce);
	CHECK(recovered == 0, "no transfer to end: the engine left alone");
}

/* the run's end with the thread stopped in a transfer through the engine: its command cancelled, before its clocks go */
static void
run_end_in_a_transfer(void)
{
	uint64_t took = 0;
	int recover, votes;

	phone("phone-cmd");
	started();
	chip = REPORT_STALLS;
	WAIT_FOR(stalled, 2000);
	CHECK(stalled, "the thread stopped in a report's read");
	if (!run_end(&took)) {
		return;
	}
	CHECK(took >= 90 && took < 500, "the thread waited for, a bounded time: %llu ms", (unsigned long long)took);
	recover = last_event(EV_RECOVER, ~0u, ~0u, 0);
	votes = last_event(EV_VOTE, ~0u, ~0u, 0);
	CHECK(recover >= 0 && recover < votes, "the engine's command cancelled (%d) before its votes are dropped (%d)", recover, votes);
	powered_off("at the run's end, in a transfer");
}

/* ... in a transfer by hand (the engine not usable): the chip deselected */
static void
run_end_in_a_transfer_by_hand(void)
{
	uint64_t took = 0;
	int raised, restored;

	phone("phone-cmd");
	geni_problem = "the serial engine's FIFO interface is disabled";
	stall_on_select = true;
	hakios_fts_init();
	WAIT_FOR(stalled, 5000);
	CHECK(stalled, "the thread stopped with the chip selected: %s", log_text);
	if (!run_end(&took)) {
		return;
	}
	CHECK(took >= 90 && took < 500, "the thread waited for, a bounded time: %llu ms", (unsigned long long)took);
	raised = last_event(EV_IO, pin[P_CS], 2, 0);
	restored = last_event(EV_CFG, pin[P_CS], cfg_found[P_CS], 0);
	CHECK(raised >= 0 && raised < restored && last_event(EV_IO, pin[P_CS], 0, 0) < raised,
	    "chip select raised (%d) before its pin is given back (%d)", raised, restored);
	CHECK(recovered == 0, "no engine to cancel");
	powered_off("at the run's end, in a transfer by hand");
}

/* the run's end on a phone where the driver did nothing: nothing done */
static void
run_end_untouched(void)
{
	uint64_t took = 0;

	phone("phone-cmd");
	boot_args = "";
	hakios_fts_init();
	if (!run_end(&took)) {
		return;
	}
	CHECK(evs == 0 && took < 10, "nothing touched: %u events, %llu ms", evs, (unsigned long long)took);
}

/* another chip answers: bring-up fails, by the engine and by hand, and the controller is powered off */
static void
bring_up_fails(void)
{
	phone("phone-cmd");
	chip = ANOTHER_ID;
	start();
	CHECK(logged("touchscreen unavailable"), "given up: %s", log_text);
	WAIT_FOR(logged("fts: powered off"), 5000);  /* (said after "unavailable") */
	powered_off("after a failed bring-up");
	no_more_polling("after a failed bring-up");
}

/* RPMh does not switch the analog supply on: the I/O supply's vote withdrawn again, the pins as found */
static void
analog_supply_refused(void)
{
	phone("phone-cmd");
	refused_supply = supply_base(hakios_board()->touch.analog_supply);
	start();
	CHECK(logged("touchscreen unavailable"), "given up: %s", log_text);
	WAIT_FOR(logged("fts: powered off"), 5000);  /* (said after "unavailable") */
	powered_off("after the analog supply was refused");
}

/* reports that keep failing, four troubles in a row: the polling given up, and the controller powered off */
static void
four_troubles_stop(void)
{
	phone("phone-cmd");
	started();
	chip = REPORTS_FAIL;
	/* (it says it stops before it powers the controller off: both waited for) */
	WAIT_FOR(logged("stopped after repeated controller errors") && logged("fts: powered off"), 20000);
	CHECK(logged("stopped after repeated controller errors") && times_logged("consecutive invalid reports") == 4,
	    "given up at the fourth: %s", log_text);
	powered_off("after the polling was given up");
	no_more_polling("after the polling was given up");
}

/* a trouble a minute: each recovered from and forgotten a minute later, the phone idle in between; the fifth not the end */
static void
troubles_forgotten(void)
{
	phone("phone-cmd");
	started();
	for (unsigned k = 1; k <= 4; k++) {
		unsigned read;

		chip = REPORTS_FAIL;
		WAIT_FOR(times_logged("consecutive invalid reports") >= k, 10000);
		chip = ANSWERS;
		read = reports_read;
		WAIT_FOR(reports_read >= read + 3, 3000);
		CHECK(reports_read >= read + 3, "trouble %u recovered from: %s", k, log_text);
		skew += 61 * NSEC_PER_SEC;      /* (a minute without a failure, no finger down) */
		read = reports_read;
		WAIT_FOR(reports_read >= read + 3, 3000);
	}
	CHECK(!logged("stopped after repeated controller errors"), "four troubles a minute apart: still polled: %s", log_text);
	CHECK(times_logged("previous recoveries 0/3") == 4, "each found none before it: %u of 4",
	    times_logged("previous recoveries 0/3"));
}

/* a finger down, and then its reads fail, each after 20 ms: lifted after a quarter of a second, not after 125 failed polls */
static void
lifted_when_reads_fail_slowly(void)
{
	uint64_t t0;
	int up;

	phone("phone-cmd");
	finger(500, 1000);
	started();
	WAIT_FOR(posted_of(HAKIOS_TOUCH_DOWN) >= 0, 2000);
	CHECK(posted_of(HAKIOS_TOUCH_DOWN) >= 0, "the finger down");
	t0 = real_ns();
	chip = REPORTS_FAIL_SLOWLY;
	WAIT_FOR(posted_of(HAKIOS_TOUCH_UP) >= 0, 3000);
	up = posted_of(HAKIOS_TOUCH_UP);
	CHECK(up >= 0 && posted[up].t - t0 >= 200 * 1000000ull && posted[up].t - t0 < 1000 * 1000000ull,
	    "lifted after %.2f s (wanted a quarter of a second)", up >= 0 ? (posted[up].t - t0) / 1e9 : (real_ns() - t0) / 1e9);
	CHECK(!logged("consecutive invalid reports"), "and the controller not reset for it: %s", log_text);
}

/* ... and when they fail at once */
static void
lifted_when_reads_fail(void)
{
	uint64_t t0;
	int up;

	phone("phone-cmd");
	finger(500, 1000);
	started();
	WAIT_FOR(posted_of(HAKIOS_TOUCH_DOWN) >= 0, 2000);
	t0 = real_ns();
	chip = REPORTS_FAIL;
	WAIT_FOR(posted_of(HAKIOS_TOUCH_UP) >= 0, 3000);
	up = posted_of(HAKIOS_TOUCH_UP);
	CHECK(up >= 0 && posted[up].t - t0 >= 200 * 1000000ull && posted[up].t - t0 < 600 * 1000000ull,
	    "lifted after %.2f s (wanted a quarter of a second)", up >= 0 ? (posted[up].t - t0) / 1e9 : (real_ns() - t0) / 1e9);
}

/* one read that fails is not a lift */
static void
kept_through_a_bad_read(void)
{
	phone("phone-cmd");
	finger(500, 1000);
	started();
	WAIT_FOR(posted_of(HAKIOS_TOUCH_DOWN) >= 0, 2000);
	failing_reads = 3;                      /* (a read is three tries) */
	usleep(400 * 1000);
	CHECK(failing_reads == 0 && posted_of(HAKIOS_TOUCH_UP) < 0, "the finger still down (%u tries left)", failing_reads);
}

/* a panel larger than the first phone's, by the board's coordinates: its reports taken, scaled to its size */
static void
the_boards_panel(void)
{
	phone("phone-keys");                    /* (1440 x 3200) */
	finger(1300, 3000);
	started();
	CHECK(logged("touchscreen ready, 1440x3200"), "said: %s", log_text);
	WAIT_FOR(posted_of(HAKIOS_TOUCH_DOWN) >= 0, 2000);
	int down = posted_of(HAKIOS_TOUCH_DOWN);
	CHECK(down >= 0 && posted[down].x == 1300u * 65535 / 1440 && posted[down].y == 3000u * 65535 / 3200,
	    "down where it is on that panel: %u, %u", down >= 0 ? posted[down].x : 0, down >= 0 ? posted[down].y : 0);
	finger(1440, 3200);
	WAIT_FOR(posts >= 2, 1000);
	CHECK(posts >= 2 && posted[1].type == HAKIOS_TOUCH_MOVE && posted[1].x == 65535 && posted[1].y == 65535,
	    "its corner is the corner");
	finger(1441, 10);
	WAIT_FOR(logged("rejected report"), 1000);
	CHECK(logged("rejected report"), "off its side: refused");
}

/* a board that gives no coordinates: the first phone's 1080 x 2400 */
static void
the_first_phones_panel(void)
{
	phone("phone-cmd");
	finger(1080, 2400);
	started();
	WAIT_FOR(posted_of(HAKIOS_TOUCH_DOWN) >= 0, 2000);
	int down = posted_of(HAKIOS_TOUCH_DOWN);
	CHECK(down >= 0 && posted[down].x == 65535 && posted[down].y == 65535, "its corner is the corner");
	finger(1081, 10);
	WAIT_FOR(logged("rejected report"), 1000);
	CHECK(logged("rejected report"), "off its side: refused");
}

/* the transfer by hand: the chip's data output (the board's data in) is only read, never driven */
static void
miso_never_driven(void)
{
	phone("phone-cmd");
	chip = ANOTHER_ID;
	geni_problem = "the serial engine's FIFO interface is disabled";
	start();
	CHECK(logged("trying GPIO SPI"), "by hand: %s", log_text);
	CHECK(miso_driven == 0, "gpio%u configured as an output %u times", pin[P_DATA_IN], miso_driven);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "run_end_powers_off", run_end_powers_off }, { "run_end_in_a_transfer", run_end_in_a_transfer },
		{ "run_end_in_a_transfer_by_hand", run_end_in_a_transfer_by_hand }, { "run_end_untouched", run_end_untouched },
		{ "bring_up_fails", bring_up_fails }, { "analog_supply_refused", analog_supply_refused },
		{ "four_troubles_stop", four_troubles_stop }, { "troubles_forgotten", troubles_forgotten },
		{ "lifted_when_reads_fail_slowly", lifted_when_reads_fail_slowly }, { "lifted_when_reads_fail", lifted_when_reads_fail },
		{ "kept_through_a_bad_read", kept_through_a_bad_read }, { "the_boards_panel", the_boards_panel },
		{ "the_first_phones_panel", the_first_phones_panel }, { "miso_never_driven", miso_never_driven },
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
			fprintf(stderr, "%-32s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
