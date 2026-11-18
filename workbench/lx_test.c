/*
 * The kernel's side of the Wi-Fi driver's object (drivers/board/lx.c), run as a host program: the radio made the link
 * behind en0, with the network stack replaced by a stand-in that can refuse the link, and the object's lx_transmit()
 * by the one a kernel without the object has. What is tested: an attach that the stack refused is tried again, and
 * only the attach -- the queue of frames to send and the thread that takes them to the driver are made once, so that
 * one thread takes each frame out once (by lx.c's own count of the frames taken out: the lx_transmit() it calls is in
 * the same file, and cannot be replaced by one that counts); and a link that is attached is not attached again.
 *
 *   lx_test SCENARIO      one scenario a process: the file keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"
#define KERNEL 1
#include <sys/board/input.h>
#include <sys/board/net.h>
#include <sys/board/sta.h>

/* an mbuf: the frame, in one piece */
struct fake_mbuf {
	const uint8_t   *data;
	size_t          length;
};

errno_t
mbuf_copydata(mbuf_t packet, size_t offset, size_t length, void *to)
{
	if (offset + length > packet->length) {
		return 22;
	}
	memcpy(to, packet->data + offset, length);
	return 0;
}

/* ---- the network stack: refuses the link as often as a scenario says, then takes it -------------------------- */

static unsigned attaches, refusals;
static const struct hakios_net_link *attached;

bool
hakios_net_attach(const struct hakios_net_link *link)
{
	attaches++;
	if (refusals != 0) {
		refusals--;
		return false;
	}
	attached = link;
	return true;
}

void hakios_net_input(const uint8_t *frame __unused, size_t length __unused) { }
void hakios_net_link_state(bool up __unused) { }

/* ---- the threads named by the file (the fake kernel's own call names nothing): the queue's taker is counted ---- */

static unsigned senders;

#define thread_set_thread_name named
static void
named(thread_t thread __unused, const char *name)
{
	if (strcmp(name, "wifi-send") == 0) {
		__atomic_fetch_add(&senders, 1, __ATOMIC_RELAXED);
	}
}

/* ---- what else lx.c calls: the radio's side of board/wifi.c, the station, the GIC; none of it answers here ---- */

int hakios_wifi_say(const char *format __unused, ...) { return 0; }
int hakios_wifi_tell(const char *format __unused, ...) { return 0; }
uint32_t hakios_wifi_option(const char *name __unused) { return 0; }
bool hakios_wifi_radio_map(uint64_t physical __unused, uint64_t size __unused, unsigned long long *address __unused) { return false; }
bool hakios_wifi_radio_window(uint64_t *window __unused, uint64_t *size __unused) { return false; }
const uint8_t *hakios_wifi_driver_file(const char *name __unused, uint64_t *size __unused) { return NULL; }
bool hakios_wifi_dsp_is_up(void) { return false; }
bool hakios_wifi_radio_address(uint8_t *address __unused) { return false; }
bool hakios_wifi_network(uint8_t *n __unused, unsigned *nl __unused, uint8_t *p __unused, unsigned *pl __unused) { return false; }
bool hakios_wifi_radio_msi(unsigned long long *address __unused, uint32_t *data __unused, uint32_t *first __unused) { return false; }
const struct hakios_board *hakios_board(void) { return NULL; }
void hakios_sta_attach(const struct hakios_sta_ops *ops __unused) { }
void hakios_sta_lost(int error __unused) { }
vm_offset_t ml_io_map_unmappable(vm_offset_t phys __unused, vm_size_t size __unused, unsigned int flags __unused) { return 0; }
void ml_io_unmap(vm_offset_t address __unused, vm_size_t size __unused) { }
uint64_t kvtophys(vm_offset_t va) { return va; }
void read_random(void *buffer, unsigned size) { memset(buffer, 0x5a, size); }
kern_return_t gic_v3_spi_attach(uint32_t intid __unused, bool edge __unused, void (*handler)(void *, uint32_t) __unused,
    void *context __unused)
{
	return KERN_FAILURE;
}
void gic_v3_spi_mask(uint32_t intid __unused) { }
void gic_v3_spi_unmask(uint32_t intid __unused) { }
void gic_v3_spi_detach(uint32_t intid __unused) { }
uint64_t gic_v3_spi_count(uint32_t intid __unused) { return 0; }

/* (what lx.c has of XNU's that the fake kernel has not: the interrupts' part, which no scenario reaches) */
#define VM_WIMG_IO                      2
#define THREAD_NULL                     ((thread_t)0)
#define KERN_NAME_EXISTS                13
#define trunc_page(x)                   ((x) & ~(vm_offset_t)PAGE_MASK)
#define os_atomic_or(p, v, order)       __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define os_atomic_xchg(p, v, order)     __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define os_atomic_store(p, v, order)    __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define panic(...)                      abort()
#include "board/lx.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

static const unsigned char address[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

static uint32_t
taken_out(void)
{
	uint32_t out;

	lck_mtx_lock(&lx_queue_lock);
	out = lx_queue_out;
	lck_mtx_unlock(&lx_queue_lock);
	return out;
}

/* frames through the link, as the stack sends them: each is to be taken out of the queue once */
static void
send_frames(unsigned count)
{
	uint8_t frame[60];
	struct fake_mbuf packet = { frame, sizeof(frame) };

	for (unsigned i = 0; i < count; i++) {
		memset(frame, (int)i, sizeof(frame));
		CHECK(attached->transmit(&packet, sizeof(frame)), "frame %u is queued", i);
		usleep(1000);
	}
}

/* ---- scenarios -------------------------------------------------------------------------------------- */

/* The stack refuses the link once (en0 has one already, for a while): the second call attaches it, on the same queue. */
static void
refused_then_attached(void)
{
	struct lx_frame *queue;
	unsigned started = fake_threads_started;

	refusals = 1;
	lx_os_net_attach(address);
	CHECK(attaches == 1 && !lx_link_attached, "the first attach is refused (%u attaches)", attaches);
	queue = lx_queue;
	CHECK(queue != NULL, "the queue is made");
	lx_os_net_attach(address);
	CHECK(attaches == 2 && lx_link_attached && attached == &lx_link, "the second attach is taken (%u attaches)", attaches);
	CHECK(lx_queue == queue, "the queue is the first call's, not a second one");
	CHECK(fake_threads_started - started == 1 && senders == 1, "one thread takes the frames (%u started, %u named wifi-send)",
	    fake_threads_started - started, senders);
	CHECK(memcmp(lx_link.address, address, sizeof(address)) == 0, "the link has the radio's address");

	send_frames(40);
	WAIT_FOR(taken_out() >= 40, 2000);
	usleep(50000);
	CHECK(taken_out() == 40 && lx_queue_in == 40, "each frame is taken out once: %u in, %u out", lx_queue_in, taken_out());
	CHECK(lx_queue_dropped == 0, "none dropped (%llu)", (unsigned long long)lx_queue_dropped);
}

/* A link that is attached is left as it is: no attach, no thread more. */
static void
attached_once(void)
{
	unsigned started = fake_threads_started;

	lx_os_net_attach(address);
	lx_os_net_attach(address);
	lx_os_net_attach(address);
	CHECK(attaches == 1 && lx_link_attached, "one attach (%u)", attaches);
	CHECK(fake_threads_started - started == 1 && senders == 1, "one thread (%u started)", fake_threads_started - started);
	send_frames(8);
	WAIT_FOR(taken_out() >= 8, 2000);
	usleep(50000);
	CHECK(taken_out() == 8, "each frame is taken out once: %u out", taken_out());
}

static const struct {
	const char *name;
	void (*run)(void);
} scenarios[] = {
	{ "refused_then_attached", refused_then_attached },
	{ "attached_once", attached_once },
};

int
main(int argc, char **argv)
{
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
