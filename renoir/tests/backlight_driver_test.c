/*
 * The backlight driver itself (bsd/dev/hakios_backlight.c from renoir/kernel/patches/0023-universal-backlight.patch),
 * run as a host program: the kernel's interfaces are renoir/tests/fakekernel, the registers a model of the
 * hardware's (a Qualcomm DSI controller 2.5 and the display's clocks, as Linux's SDE driver and the phones'
 * device trees describe them). renoir/tests/run.sh builds it for the host and for aarch64, the second run
 * under qemu-aarch64 (so the driver's arm64 code runs, its barriers included).
 *
 *   backlight_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "fakekernel.h"

/* ---- the fake kernel ---------------------------------------------------------------------------- */

static const char *boot_args = "";
static const char *board = "";
static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t woken;
static uint64_t wake_generation;
static __thread void *wait_event;
static __thread uint64_t wait_until_ns, wait_generation;
static char log_text[1 << 16];
static size_t log_length;

/* the waits' deadlines are by the monotonic clock, as mach_absolute_time() below */
__attribute__((constructor)) static void
fake_kernel_init(void)
{
	pthread_condattr_t attributes;

	pthread_condattr_init(&attributes);
	pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	pthread_cond_init(&woken, &attributes);
}

uint64_t
mach_absolute_time(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * NSEC_PER_SEC + (uint64_t)t.tv_nsec;
}

void
absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result)
{
	*result = abstime;
}

void
delay(int usec)
{
	usleep((useconds_t)(usec > 0 ? usec : 0));
}

void
assert_wait(void *event, int interruptible __unused)
{
	pthread_mutex_lock(&big);
	wait_event = event;
	wait_until_ns = 0;
	wait_generation = wake_generation;
	pthread_mutex_unlock(&big);
}

void
assert_wait_timeout(void *event, int interruptible __unused, uint32_t interval, uint64_t scale)
{
	pthread_mutex_lock(&big);
	wait_event = event;
	wait_until_ns = mach_absolute_time() + interval * scale;
	wait_generation = wake_generation;
	pthread_mutex_unlock(&big);
}

wait_result_t
thread_block(thread_continue_t continuation __unused)
{
	pthread_mutex_lock(&big);
	while (wake_generation == wait_generation) {
		if (wait_until_ns != 0) {
			struct timespec t = { (time_t)(wait_until_ns / NSEC_PER_SEC), (long)(wait_until_ns % NSEC_PER_SEC) };

			if (mach_absolute_time() >= wait_until_ns) {
				break;
			}
			pthread_cond_timedwait(&woken, &big, &t);
		} else {
			pthread_cond_wait(&woken, &big);
		}
	}
	wait_event = NULL;
	pthread_mutex_unlock(&big);
	return 0;
}

void
thread_wakeup(void *event __unused)
{
	pthread_mutex_lock(&big);
	wake_generation++;
	pthread_cond_broadcast(&woken);
	pthread_mutex_unlock(&big);
}

struct start { thread_continue_t fn; void *arg; };

static void *
thread_main(void *p)
{
	struct start s = *(struct start *)p;

	free(p);
	s.fn(s.arg, 0);
	return NULL;
}

kern_return_t
kernel_thread_start(thread_continue_t fn, void *arg, thread_t *thread)
{
	pthread_t t;
	struct start *s = malloc(sizeof(*s));

	s->fn = fn;
	s->arg = arg;
	*thread = NULL;
	return pthread_create(&t, NULL, thread_main, s) == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

void thread_deallocate(thread_t thread __unused) {}
void thread_set_thread_name(thread_t thread __unused, const char *name __unused) {}
bool ml_get_interrupts_enabled(void) { return false; }      /* (everything to the loader's log only) */

void
loader_log_putc(char c)
{
	pthread_mutex_lock(&big);
	if (log_length + 1 < sizeof(log_text)) {
		log_text[log_length++] = c;
		log_text[log_length] = 0;
	}
	pthread_mutex_unlock(&big);
	if (getenv("BL_VERBOSE")) {
		fputc(c, stderr);
	}
}

static bool
logged(const char *text)
{
	bool found;

	pthread_mutex_lock(&big);
	found = strstr(log_text, text) != NULL;
	pthread_mutex_unlock(&big);
	return found;
}

bool
redwood_board_is(const char *name)
{
	return strcmp(name, board) == 0;
}

/* "name=value" among the boot-args: a number (decimal or 0x hex) or a string */
bool
PE_parse_boot_argn(const char *name, void *value, int size)
{
	size_t n = strlen(name);

	for (const char *a = boot_args; *a != 0;) {
		const char *end = strchr(a, ' ');
		size_t length = end ? (size_t)(end - a) : strlen(a);

		if (length > n && strncmp(a, name, n) == 0 && a[n] == '=') {
			char text[64];
			size_t vlen = length - n - 1;

			memcpy(text, a + n + 1, vlen < sizeof(text) - 1 ? vlen : sizeof(text) - 1);
			text[vlen < sizeof(text) - 1 ? vlen : sizeof(text) - 1] = 0;
			if (text[0] >= '0' && text[0] <= '9') {
				uint64_t number = strtoull(text, NULL, 0);

				memcpy(value, &number, (size_t)size < sizeof(number) ? (size_t)size : sizeof(number));
			} else {
				strncpy(value, text, (size_t)size);
				((char *)value)[size - 1] = 0;
			}
			return true;
		}
		a += length;
		while (*a == ' ') {
			a++;
		}
	}
	return false;
}

/* sysctl: a table by name ("_hw_backlight.level") */
static struct sysctl_oid *oids[32];
static int oid_count;

void
fake_sysctl_register(struct sysctl_oid *oid)
{
	oids[oid_count++] = oid;
}

static struct sysctl_oid *
oid_named(const char *name)
{
	for (int i = 0; i < oid_count; i++) {
		const char *dot = strrchr(oids[i]->name, '.');

		if (dot != NULL && strcmp(dot + 1, name) == 0) {
			return oids[i];
		}
	}
	fprintf(stderr, "no sysctl %s\n", name);
	exit(2);
}

int
sysctl_io_number(struct sysctl_req *req, long long value, size_t size, void *pointer, int *changed)
{
	if (req->oldptr != NULL) {
		memcpy(req->oldptr, &value, size);      /* (little-endian host: the low bytes) */
		req->oldidx = size;
	}
	if (req->newptr != NULL) {
		memcpy(pointer, req->newptr, size);
		*changed = 1;
	}
	return 0;
}

int
fake_sysctl_out(struct sysctl_req *req, const void *data, size_t size)
{
	if (req->oldptr != NULL && size <= req->oldlen) {
		memcpy(req->oldptr, data, size);
	}
	req->oldidx = size;
	return 0;
}

static uint32_t
get(const char *name)
{
	struct sysctl_oid *oid = oid_named(name);
	uint32_t value = 0;
	struct sysctl_req req = { NULL, 0, &value, sizeof(value), 0 };

	if (oid->handler != NULL) {
		oid->handler(oid, oid->arg1, oid->arg2, &req);
	} else {
		value = *(volatile uint32_t *)oid->arg1;
	}
	return value;
}

static const char *
get_string(const char *name)
{
	static char text[32];
	struct sysctl_oid *oid = oid_named(name);
	struct sysctl_req req = { NULL, 0, text, sizeof(text), 0 };

	oid->handler(oid, oid->arg1, oid->arg2, &req);
	return text;
}

static void
set(const char *name, uint32_t value)
{
	struct sysctl_oid *oid = oid_named(name);
	struct sysctl_req req = { &value, sizeof(value), NULL, 0, 0 };

	if (!(oid->flags & CTLFLAG_ANYBODY)) {
		fprintf(stderr, "sysctl %s is not writable by iOS's processes\n", name);
		exit(2);
	}
	oid->handler(oid, oid->arg1, oid->arg2, &req);
}

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
	bool cmd_mode_during_send;
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
		m.sent[m.sent_n++ % 4096] = (uint32_t)b[5] << 8 | b[6];     /* the panels here: MSB first */
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
bl_test_rd(vm_offset_t a)
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
bl_test_wr(vm_offset_t a, uint32_t v)
{
	const uint32_t status_bits = 0x1u | 0x100u | 0x400u | 0x1000u | 0x4000u | 0x10000u | 0x100000u | 0x1000000u |
	    0x10000000u | 0x40000000u;

	if (a >= DSI_BASE && a < DSI_BASE + 0x400) {
		m.accesses++;
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
			}
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
	return phys;            /* (an address of the model's: bl_test_rd/wr tell them apart) */
}

/* ---- the driver --------------------------------------------------------------------------------- */

#define BL_TEST_MMIO 1
#include "hakios_backlight.c"

/* ---- the scenarios ------------------------------------------------------------------------------ */

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* wait (up to `ms`) until `cond` holds */
#define WAIT_FOR(cond, ms) do { for (int waited_ = 0; !(cond) && waited_ < (ms); waited_ += 5) usleep(5000); } while (0)

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
	c.gdsc_offset = strcmp(name, "renoir") == 0 ? 0x3000 : 0x1004;
	c.ahb_offset = strcmp(name, "renoir") == 0 ? 0x2080 : 0x1050;
}

static void
renoir_dcs(void)
{
	uint32_t ctrl0;

	phone("renoir", false);
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
	CHECK(get("shown") == 1000, "shown %u", get("shown"));
	CHECK(get("raw") == 2047, "raw %u", get("raw"));
	set("level", 0);
	WAIT_FOR(last_sent() == 1, 2000);
	CHECK(last_sent() == 1, "lowest: %u", last_sent());
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

static void
redwood_video(void)
{
	phone("redwood", true);
	boot_args = "backlight=2 bl_level=1000";
	hakios_backlight_init();
	WAIT_FOR(m.sent_n >= 1, 1000);
	CHECK(last_sent() == 2047, "redwood's top without HBM: %u", last_sent());
	CHECK(m.cmd_mode_during_send, "the command engine on for the command");
	CHECK(!(m.ctrl & 4), "... and off again: %#x", m.ctrl);
	CHECK(m.errors == 0, "%s", m.error);
}

static void
look_only(void)
{
	phone("renoir", false);
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
	phone("renoir", false);
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
	phone("renoir", false);
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
	phone("renoir", false);
	m.deaf = true;
	boot_args = "backlight=2";
	hakios_backlight_init();
	WAIT_FOR(logged("NOT SENT"), 2000);
	CHECK(logged("NOT SENT"), "a command the controller did not send is said");
	set("level", 100);
	usleep(300000);
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

static void
automatic_brightness(void)
{
	phone("renoir", false);
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
	WAIT_FOR(get("shown") == 730, 2000);
	CHECK(get("shown") == 730 && get("offset") == 0, "by hand again: %u", get("shown"));
	CHECK(m.errors == 0, "%s", m.error);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "renoir_dcs", renoir_dcs }, { "redwood_video", redwood_video }, { "look_only", look_only },
		{ "off_by_default", off_by_default }, { "display_unpowered", display_unpowered },
		{ "deaf_controller", deaf_controller }, { "soft_unknown_board", soft_unknown_board }, { "automatic", automatic_brightness },
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
