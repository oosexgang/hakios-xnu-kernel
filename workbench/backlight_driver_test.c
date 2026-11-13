/*
 * The backlight driver itself (kernel/overlay/bsd/dev/board/backlight.c),
 * run as a host program: the kernel's interfaces are workbench/fakekernel, the registers a model of the
 * hardware's (a Qualcomm DSI controller 2.5 and the display's clocks, as Linux's SDE driver and the phones'
 * device trees describe them). workbench/run.sh builds it for the host and for aarch64, the second run
 * under qemu-aarch64 (so the driver's arm64 code runs, its barriers included).
 *
 *   backlight_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

/* ---- the hardware's model ----------------------------------------------------------------------- */

#define DSI_BASE        0x0ae94000u
#define GCC_BASE        0x00100000u
#define DISPCC_BASE     0x0af00000u

struct dsi_model {
	uint32_t version, ctrl, status, dma_ctrl, length, tpg, int_enables, int_status;
	uint32_t fifo[16];
	unsigned fifo_n;
	bool deaf;                      /* never says a command was sent */
	unsigned reads_since_clear;     /* (video mode: a frame ends a few reads after VIDEO_DONE was cleared) */
	unsigned busy_reads;            /* (command mode: so many reads of STATUS busy after a command) */
	/* what the panel was sent */
	uint32_t sent[4096];
	unsigned sent_n;
	unsigned errors;                /* something done out of the protocol */
	char error[160];
	unsigned accesses;
	unsigned writes;                /* writes into the controller's registers */
	bool cmd_mode_during_send;
	bool to_be_emptied, reset_asked;        /* the FIFO after a command: reset before the next */
} m;

struct clock_model {
	uint32_t gcc_disp_ahb, gdsc, ahb;
	uint32_t gdsc_offset, ahb_offset;
} c;

static void
model_error(const char *what)
{
	m.errors++;
	snprintf(m.error, sizeof(m.error), "%s", what);
	fprintf(stderr, "MODEL: %s\n", what);
}

/* the command in the FIFO, read as the controller would, then what the panel does with it */
static void
model_trigger(void)
{
	uint8_t b[64];
	unsigned type;

	if ((m.ctrl & 1) == 0) {
		model_error("triggered with the controller off");
	}
	if ((m.ctrl & 4) == 0) {
		model_error("triggered without the command engine");
	}
	if (m.tpg != ((1u << 1) | (1u << 2) | (3u << 16))) {
		model_error("triggered without the TPG DMA FIFO selected");
	}
	if ((m.dma_ctrl & (1u << 28)) == 0 || (m.dma_ctrl & (3u << 30)) != 0) {
		model_error("triggered without embedded mode, or broadcast");
	}
	if (m.dma_ctrl & (1u << 26)) {
		model_error("triggered in low power mode: a panel takes its brightness at high speed");
	}
	/* a command goes between two frames: after one's end (video), not while one is on its way (command) */
	if ((m.ctrl & 2) && !(m.int_status & 0x10000)) {
		model_error("triggered in the middle of a frame (video mode)");
	}
	if (!(m.ctrl & 2) && m.busy_reads != 0) {
		model_error("triggered while a frame is on its way to the panel (command mode)");
	}
	if (m.to_be_emptied) {
		model_error("the FIFO was not emptied after the command before");
	}
	m.to_be_emptied = true;
	if (m.fifo_n == 0 || (m.fifo_n & 1) != 0 || m.length == 0 || m.length > m.fifo_n * 4 || (m.length & 3) != 0) {
		model_error("FIFO words and length do not agree");
	}
	m.cmd_mode_during_send = (m.ctrl & 4) != 0;
	for (unsigned i = 0; i < m.fifo_n && i < 16; i++) {
		memcpy(b + 4 * i, &m.fifo[i], 4);
	}
	type = b[2] & 0x3f;
	if ((b[3] & 0x80) == 0) {
		model_error("not marked the last command");
	}
	if (type == 0x39) {
		unsigned wc = b[0] | b[1] << 8;

		if (wc != 3 || (b[3] & 0x40) == 0 || b[4] != 0x51) {
			model_error("long write that is not 0x51 with two bytes");
		}
		m.sent[m.sent_n++ % 4096] = (uint32_t)b[5] << 8 | b[6];     /* the panels here: MSB first (0: dark) */
	} else if (type == 0x15) {
		if (b[0] != 0x51) {
			model_error("short write that is not 0x51");
		}
		m.sent[m.sent_n++ % 4096] = b[1];
	} else {
		model_error("neither a DCS short nor a long write");
	}
	m.fifo_n = 0;
	if (!m.deaf) {
		m.int_status |= 1;              /* CMD_MODE_DMA_DONE */
	}
	m.busy_reads = 3;
}

uint32_t
hakios_test_rd(vm_offset_t a)
{
	if (a >= DSI_BASE && a < DSI_BASE + 0x400) {
		m.accesses++;
		switch (a - DSI_BASE) {
		case 0x000: return m.version;
		case 0x004: return m.ctrl;
		case 0x008:
			if (m.busy_reads) {
				m.busy_reads--;
				return m.status | 4;
			}
			return m.status;
		case 0x03c: return m.dma_ctrl;
		case 0x110:
			if ((m.ctrl & 2) && !(m.int_status & 0x10000) && ++m.reads_since_clear > 3) {
				m.int_status |= 0x10000;        /* the frame ends */
			}
			return m.int_status | m.int_enables;
		default: return 0;
		}
	}
	if (a == GCC_BASE + 0x27004) {
		return c.gcc_disp_ahb;
	}
	if (a == DISPCC_BASE + c.gdsc_offset) {
		return c.gdsc;
	}
	if (a == DISPCC_BASE + c.ahb_offset) {
		return c.ahb;
	}
	model_error("read of a register the driver has no business with");
	return 0;
}

void
hakios_test_wr(vm_offset_t a, uint32_t v)
{
	const uint32_t status_bits = 0x1u | 0x100u | 0x400u | 0x1000u | 0x4000u | 0x10000u | 0x100000u | 0x1000000u |
	    0x10000000u | 0x40000000u;

	if (a >= DSI_BASE && a < DSI_BASE + 0x400) {
		m.accesses++;
		m.writes++;
		switch (a - DSI_BASE) {
		case 0x004:
			m.ctrl = v;
			return;
		case 0x03c:
			m.dma_ctrl = v;
			return;
		case 0x04c:
			m.length = v;
			return;
		case 0x090:
			if (v == 1) {
				model_trigger();
			}
			return;
		case 0x110:
			m.int_status &= ~(v & status_bits);     /* written 1: cleared */
			m.int_enables = v & ~status_bits;
			if (v & 0x10000) {
				m.reads_since_clear = 0;
			}
			return;
		case 0x15c:
			m.tpg = v;
			return;
		case 0x17c:
			if (m.tpg == 0) {
				model_error("FIFO written with the TPG off");
			}
			if (m.fifo_n < 16) {
				m.fifo[m.fifo_n++] = v;
			}
			return;
		case 0x1ec:
			if (v == 0) {
				m.fifo_n = 0;
				m.to_be_emptied = m.to_be_emptied && !m.reset_asked;
			}
			m.reset_asked = v == 1;
			return;
		default:
			model_error("write to a DSI register the driver has no business with");
			return;
		}
	}
	if (a == GCC_BASE + 0x27004) {
		c.gcc_disp_ahb = v;
		return;
	}
	if (a == DISPCC_BASE + c.ahb_offset) {
		c.ahb = v & 1 ? v & ~0x80000000u : v | 0x80000000u;     /* on when asked */
		return;
	}
	model_error("write to a register the driver has no business with");
}

vm_offset_t
ml_io_map(vm_offset_t phys, vm_size_t size __unused)
{
	return phys;            /* (an address of the model's: hakios_test_rd/wr tell them apart) */
}

/* ---- the driver, and the board table it reads (devices/, through tools/boards.py) -------------- */

#include "board/board.c"
#define HAKIOS_MMIO_TEST 1
#include "board/backlight.c"

/* ---- the scenarios ------------------------------------------------------------------------------ */

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* wait (up to `ms`) until `cond` holds */
#define WAIT_FOR(cond, ms) do { for (int waited_ = 0; !(cond) && waited_ < (ms); waited_ += 5) usleep(5000); } while (0)

/*
 * Wait until the driver has stopped talking to the controller (no access for a tenth of a second): a
 * command is noted by the model the moment it is triggered, and the driver still has the FIFO to empty
 * and the controller's setup to put back after that.
 */
static void
settle(void)
{
	for (int tries = 0; tries < 40; tries++) {
		unsigned before = m.accesses;

		usleep(100000);
		if (m.accesses == before) {
			return;
		}
	}
}

static uint32_t
last_sent(void)
{
	return m.sent_n ? m.sent[(m.sent_n - 1) % 4096] : 0;
}

static void
phone(const char *name, bool video)
{
	board = name;
	m.version = 0x20050000;
	m.ctrl = video ? 0x1f3 : 0x1f5;         /* on, lanes, video or command */
	c.gcc_disp_ahb = 0x8000000b & ~1u;      /* (as the firmware leaves it: asked off) */
	c.gdsc = 0x80000000;
	c.ahb = 0x80000000;                     /* off */
	c.gdsc_offset = strcmp(name, "phone-cmd") == 0 ? 0x3000 : 0x1004;
	c.ahb_offset = strcmp(name, "phone-cmd") == 0 ? 0x2080 : 0x1050;
}

static void
command_panel(void)
{
	uint32_t ctrl0;

	phone("phone-cmd", false);
	ctrl0 = m.ctrl;
	boot_args = "backlight=2 bl_level=500";
	hakios_backlight_init();
	CHECK(strcmp(get_string("backend"), "dcs") == 0, "%s", get_string("backend"));
	CHECK(logged("version 2.5"), "the controller's version logged");
	CHECK(c.gcc_disp_ahb & 1, "the clock controller's bus clock asked for");
	CHECK(!(c.ahb & 0x80000000u), "the display registers' clock running");
	WAIT_FOR(m.sent_n >= 1, 1000);
	CHECK(m.sent_n >= 1 && last_sent() == bl_level_to_raw(500, 1, 2047), "boot level sent: %u", last_sent());
	set("level", 1000);
	WAIT_FOR(last_sent() == 2047, 2000);
	CHECK(last_sent() == 2047, "full: %u", last_sent());
	WAIT_FOR(get("raw") == 2047, 1000);     /* (the driver notes it once the controller says it was sent) */
	CHECK(get("shown") == 1000, "shown %u", get("shown"));
	CHECK(get("raw") == 2047, "raw %u", get("raw"));
	set("level", 0);
	WAIT_FOR(last_sent() == 1, 2000);
	CHECK(last_sent() == 1, "lowest: %u", last_sent());
	settle();
	/* the ramp went down in steps, not in one jump, and never out of the panel's range */
	CHECK(m.sent_n >= 4, "%u commands", m.sent_n);
	for (unsigned i = 0; i < m.sent_n; i++) {
		CHECK(m.sent[i] >= 1 && m.sent[i] <= 2047, "command %u: %u", i, m.sent[i]);
	}
	CHECK(m.ctrl == ctrl0, "the controller's setup as it was: %#x", m.ctrl);
	CHECK(m.tpg == 0 && m.fifo_n == 0, "the TPG off and the FIFO empty after");
	CHECK(m.errors == 0, "%s", m.error);
	hakios_backlight_quiesce();
	CHECK(last_sent() == 0x133, "left at the init level for the loader: %u", last_sent());
}

/* what the firmware left in the controller's setup for commands is put aside for a command and put back */
static void
setup_put_back(void)
{
	const uint32_t left = (3u << 30) | (1u << 26) | 0x55;   /* broadcast to both controllers, low power, and more */

	phone("phone-cmd", false);
	m.dma_ctrl = left;
	boot_args = "backlight=2 bl_level=500";
	hakios_backlight_init();
	WAIT_FOR(m.sent_n >= 1, 1000);
	set("level", 900);
	WAIT_FOR(last_sent() == bl_level_to_raw(900, 1, 2047), 3000);
	settle();
	CHECK(last_sent() == bl_level_to_raw(900, 1, 2047) && m.sent_n >= 2, "commands were sent: %u, the last %u", m.sent_n, last_sent());
	CHECK(m.errors == 0, "each to this controller alone, at high speed: %s", m.error);
	CHECK(m.dma_ctrl == left, "and the setup is as the firmware left it: %#x", m.dma_ctrl);
}

/* a controller without the command FIFO (before 2.2), and one the firmware did not leave running: not touched */
static void
controller_not_usable(void)
{
	phone("phone-cmd", false);
	m.version = 0x10030000;
	boot_args = "backlight=2 bl_level=500";
	hakios_backlight_init();
	usleep(300000);
	CHECK(logged("not a DSI controller of 2.2 or later"), "an old controller is said to be one: %s", log_text);
	CHECK(m.sent_n == 0 && m.writes == 0 && m.errors == 0, "and nothing is written into it: %u commands, %u writes", m.sent_n, m.writes);
	set("level", 1000);
	usleep(300000);
	CHECK(m.sent_n == 0 && m.writes == 0, "nor later, when a level is asked for");
}

static void
controller_off(void)
{
	phone("phone-cmd", false);
	m.ctrl &= ~1u;
	boot_args = "backlight=2 bl_level=500";
	hakios_backlight_init();
	usleep(300000);
	CHECK(logged("the controller is off (the firmware did not leave the panel lit through it)"), "said: %s", log_text);
	CHECK(m.sent_n == 0 && m.writes == 0 && m.errors == 0, "nothing is written into it: %u commands, %u writes", m.sent_n, m.writes);
}

/* a board file that says command mode on a phone whose controller runs a video panel: the controller is believed */
static void
panel_of_the_other_mode(void)
{
	uint32_t ctrl0;

	phone("phone-cmd", true);
	ctrl0 = m.ctrl;
	boot_args = "backlight=2 bl_level=500";
	hakios_backlight_init();
	CHECK(logged("the controller runs a video-mode panel, the profile says command mode: going by the controller"), "said: %s", log_text);
	WAIT_FOR(m.sent_n >= 1, 1000);
	set("level", 1000);
	WAIT_FOR(last_sent() == 2047, 3000);
	settle();
	CHECK(last_sent() == 2047, "the level is sent: %u", last_sent());
	CHECK(m.errors == 0, "as to a video panel, between two frames: %s", m.error);
	CHECK(m.cmd_mode_during_send && m.ctrl == ctrl0, "with the command engine on for the command only: %#x", m.ctrl);
}

static void
video_panel(void)
{
	phone("phone-video", true);
	boot_args = "backlight=2 bl_level=1000";
	hakios_backlight_init();
	WAIT_FOR(last_sent() == 2047, 2000);
	settle();
	CHECK(last_sent() == 2047, "the top without the high-brightness range: %u", last_sent());
	CHECK(m.cmd_mode_during_send, "the command engine on for the command");
	CHECK(!(m.ctrl & 4), "... and off again: %#x", m.ctrl);
	CHECK(m.errors == 0, "%s", m.error);
}

static void
look_only(void)
{
	phone("phone-cmd", false);
	boot_args = "backlight=1";
	hakios_backlight_init();
	usleep(200000);
	CHECK(strcmp(get_string("backend"), "none") == 0, "%s", get_string("backend"));
	CHECK(logged("looking only") && logged("version 2.5"), "said what it found");
	CHECK(m.sent_n == 0 && m.fifo_n == 0 && m.tpg == 0, "nothing sent");
	CHECK(m.errors == 0, "%s", m.error);
}

static void
off_by_default(void)
{
	phone("phone-cmd", false);
	boot_args = "";
	hakios_backlight_init();
	usleep(100000);
	CHECK(strcmp(get_string("backend"), "none") == 0, "%s", get_string("backend"));
	CHECK(m.accesses == 0, "%u accesses to the DSI controller", m.accesses);
	CHECK(!hakios_backlight_soft(), "no software light either");
}

static void
display_unpowered(void)
{
	phone("phone-cmd", false);
	c.gdsc = 0;
	boot_args = "backlight=2";
	hakios_backlight_init();
	usleep(100000);
	CHECK(m.accesses == 0, "%u accesses to an unpowered block", m.accesses);
	CHECK(strcmp(get_string("backend"), "none") == 0, "%s", get_string("backend"));
}

static void
deaf_controller(void)
{
	phone("phone-cmd", false);
	m.deaf = true;
	boot_args = "backlight=2";
	hakios_backlight_init();
	WAIT_FOR(logged("NOT SENT"), 2000);
	CHECK(logged("NOT SENT"), "a command the controller did not send is said");
	set("level", 100);
	settle();
	CHECK(m.sent_n == 1, "and nothing more is sent: %u", m.sent_n);
	CHECK(m.tpg == 0, "the TPG off even so");
}

static int redraws;
static void redraw(void) { __atomic_fetch_add(&redraws, 1, __ATOMIC_RELAXED); }

static void
soft_unknown_board(void)
{
	board = "some-new-phone";
	boot_args = "backlight=2 bl_level=1000";
	CHECK(hakios_backlight_soft(), "a board not in the table: the light in software");
	hakios_backlight_set_redraw(redraw);
	hakios_backlight_init();
	usleep(100000);
	CHECK(hakios_backlight_dim() == 256, "full: %u", hakios_backlight_dim());
	set("level", 0);
	WAIT_FOR(hakios_backlight_dim() == 24, 2000);
	CHECK(hakios_backlight_dim() == 24, "the floor: %u", hakios_backlight_dim());
	CHECK(redraws > 0, "the display driver asked to show it again");
	CHECK(m.accesses == 0, "no hardware");
}

/*
 * A level that is asked for in the moment between the thread's last look and its sleep: the wake-up finds
 * nobody asleep. The thread must not then sleep on what it read before.
 */
static void
ask_for_300(void)
{
	set("level", 300);
}

static void
display_on(void)
{
	set("power", 1);
}

static void
asked_just_before_it_sleeps(void)
{
	phone("phone-cmd", false);
	boot_args = "backlight=2 bl_level=500";
	hakios_backlight_init();
	WAIT_FOR(m.sent_n >= 1 && get("shown") == 500, 1000);
	settle();
	fake_before_wait = ask_for_300;         /* the next time the thread has nothing to do and goes to sleep for good */
	set("level", 600);
	EVENTUALLY(fake_before_wait == NULL, 5000, "the thread came to sleep once 600 was shown");
	EVENTUALLY(get("shown") == 300 && last_sent() == bl_level_to_raw(300, 1, 2047), 5000, "and what was asked for in that moment is shown: %u",
	    get("shown"));
	/* the same moment with the display switched off: switched on again just before the thread sleeps, it is lit */
	settle();
	fake_before_wait = display_on;
	set("power", 0);
	EVENTUALLY(fake_before_wait == NULL, 5000, "the thread went dark and came to sleep");
	EVENTUALLY(last_sent() == bl_level_to_raw(300, 1, 2047) && get("shown") == 300, 5000, "and the display that was switched on in that moment is lit: %u",
	    last_sent());
	settle();
	CHECK(m.errors == 0, "%s", m.error);
}

static void
automatic_brightness(void)
{
	phone("phone-cmd", false);
	boot_args = "backlight=2 bl_level=500";
	hakios_backlight_init();
	WAIT_FOR(m.sent_n >= 1, 1000);
	set("lux", 0);
	set("auto", 1);
	WAIT_FOR(get("shown") == 60, 8000);
	CHECK(get("shown") == 60, "dark room: %u", get("shown"));
	set("lux", 30000000);                   /* sunlight */
	usleep(150000);
	CHECK(get("shown") > 60 && get("shown") < 1000, "brightening, not jumping: %u", get("shown"));
	WAIT_FOR(get("shown") == 1000, 4000);
	CHECK(get("shown") == 1000, "sun: %u", get("shown"));
	set("lux", 27000000);                   /* a cloud: within the hysteresis */
	usleep(300000);
	CHECK(get("shown") == 1000, "a flicker is not followed: %u", get("shown"));
	set("lux", 300000);                     /* an office */
	WAIT_FOR(get("shown") == 530, 10000);
	CHECK(get("shown") == 530, "office: %u", get("shown"));
	set("level", 730);                      /* the user wants it brighter */
	WAIT_FOR(get("shown") == 730, 4000);
	CHECK(get("offset") == 200, "offset %d", (int)get("offset"));
	CHECK(get("shown") == 730, "office, brighter: %u", get("shown"));
	set("lux", 0);
	WAIT_FOR(get("shown") == 260, 15000);
	CHECK(get("shown") == 260, "dark, the user's preference kept: %u", get("shown"));
	set("auto", 0);
	WAIT_FOR(get("shown") == 730 && get("offset") == 0, 2000);
	CHECK(get("shown") == 730 && get("offset") == 0, "by hand again: %u", get("shown"));
	CHECK(m.errors == 0, "%s", m.error);
}

static void
display_power(void)
{
	uint32_t lit;

	phone("phone-cmd", false);
	boot_args = "backlight=2 bl_level=600";
	hakios_backlight_init();
	lit = bl_level_to_raw(600, 1, 2047);
	WAIT_FOR(last_sent() == lit, 1000);
	set("power", 0);                        /* the side button */
	WAIT_FOR(last_sent() == 0, 1000);
	CHECK(last_sent() == 0 && get("shown") == 0, "dark: %u", last_sent());
	set("level", 900);                      /* (moved while dark: kept for later, nothing sent) */
	usleep(200000);
	CHECK(last_sent() == 0, "still dark: %u", last_sent());
	set("power", 1);
	WAIT_FOR(last_sent() == bl_level_to_raw(900, 1, 2047), 2000);
	CHECK(last_sent() == bl_level_to_raw(900, 1, 2047), "lit again at the new level: %u", last_sent());
	set("power", 2);                        /* the idle timer's dimming */
	WAIT_FOR(last_sent() == bl_level_to_raw(150, 1, 2047), 2000);    /* (shown moves first, the command follows) */
	CHECK(get("shown") == 150 && last_sent() == bl_level_to_raw(150, 1, 2047), "dimmed: %u", last_sent());
	set("power", 1);
	WAIT_FOR(get("shown") == 900, 2000);
	CHECK(get("shown") == 900, "back: %u", get("shown"));
	set("power", 0);
	WAIT_FOR(last_sent() == 0, 1000);
	hakios_backlight_quiesce();
	CHECK(last_sent() == 0x133, "the loader gets a lit panel even after dark: %u", last_sent());
	CHECK(m.errors == 0, "%s", m.error);
}

static void
soft_power(void)
{
	board = "qemu-virt";
	boot_args = "backlight=2 bl_level=1000";
	hakios_backlight_set_redraw(redraw);
	hakios_backlight_init();
	usleep(100000);
	set("power", 0);
	WAIT_FOR(hakios_backlight_dim() == 0, 1000);
	CHECK(hakios_backlight_dim() == 0, "black: %u", hakios_backlight_dim());
	set("power", 1);
	WAIT_FOR(hakios_backlight_dim() == 256, 1000);
	CHECK(hakios_backlight_dim() == 256, "lit: %u", hakios_backlight_dim());
	CHECK(m.accesses == 0, "no hardware");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "command_panel", command_panel }, { "setup_put_back", setup_put_back }, { "controller_not_usable", controller_not_usable },
		{ "controller_off", controller_off }, { "panel_of_the_other_mode", panel_of_the_other_mode }, { "video_panel", video_panel }, { "look_only", look_only },
		{ "off_by_default", off_by_default }, { "display_unpowered", display_unpowered },
		{ "deaf_controller", deaf_controller }, { "soft_unknown_board", soft_unknown_board }, { "asked_just_before_it_sleeps", asked_just_before_it_sleeps }, { "automatic", automatic_brightness },
		{ "display_power", display_power }, { "soft_power", soft_power },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			printf("%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			if (failures && !getenv("BL_VERBOSE")) {
				fputs(log_text, stdout);
			}
			return failures != 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
