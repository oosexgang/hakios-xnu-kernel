/*
 * Darwin's side of Bluetooth (drivers/board/bt_host.c), run as a host program: its kernel control
 * com.apple.uart.sk.bluetooth, called as XNU's sockets call it, in front of a controller that keeps what it is sent.
 * Skywalk is a stand-in that never opens a channel (fakekernel/skywalk), so what reaches the controller is what was
 * written to the socket. What is tested: one socket at a time -- a second is refused while one is connected, and the
 * first one's half-written packet is not touched by it; what comes from a socket that is not the connected one is
 * refused, and changes nothing.
 *
 *   bt_host_test SCENARIO      one scenario a process: the file keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"
#include <sys/kern_control.h>
#include <sys/board/bt.h>

extern char log_text[];
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

/* an mbuf: the bytes of one write, in one piece */
struct fake_mbuf {
	const uint8_t   *data;
	size_t          length;
};
mbuf_t mbuf_next(mbuf_t m __unused) { return NULL; }
size_t mbuf_len(mbuf_t m) { return m->length; }
void *mbuf_data(mbuf_t m) { return __DECONST(void *, m->data); }
void mbuf_freem(mbuf_t m __unused) { }

void read_random(void *buffer, u_int size) { memset(buffer, 0x5a, size); }

/* the service bluetoothd finds the controller by (iokit/Kernel/BoardBluetooth.cpp): nothing here */
int board_bt_publish(void) { return 0; }
void board_bt_unpublish(void) { }

/* ---- the kernel control, kept as registered, and a socket's calls into it as XNU makes them -------------------- */

static struct kern_ctl_reg registered;

errno_t
ctl_register(struct kern_ctl_reg *reg, kern_ctl_ref *ref)
{
	registered = *reg;
	*ref = &registered;
	return 0;
}

errno_t ctl_deregister(kern_ctl_ref ref __unused) { return 0; }

/* a socket of UNIT connects: 0 or the control's errno -- and, refused, its disconnect called all the same, with its own
 * unit (XNU's ctl_connect does so) */
static errno_t
connect_unit(u_int32_t unit)
{
	struct sockaddr_ctl address = { .sc_len = sizeof(address), .sc_unit = unit };
	void *context = NULL;
	errno_t error = registered.ctl_connect(&registered, &address, &context);

	if (error != 0) {
		(void)registered.ctl_disconnect(&registered, unit, context);
	}
	return error;
}

static errno_t disconnect_unit(u_int32_t unit) { return registered.ctl_disconnect(&registered, unit, NULL); }

static errno_t
write_unit(u_int32_t unit, const uint8_t *bytes, size_t length)
{
	struct fake_mbuf m = { bytes, length };

	return registered.ctl_send(&registered, unit, NULL, &m, 0);
}

/* ---- the controller: what it is sent, kept; it says nothing ------------------------------------------------- */

static pthread_mutex_t sent_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t sent_bytes[1024];
static volatile unsigned sent_length, sent_packets;

static int
controller_send(const uint8_t *h4, unsigned length)
{
	pthread_mutex_lock(&sent_lock);
	if (length <= sizeof(sent_bytes) - sent_length) {
		memcpy(sent_bytes + sent_length, h4, length);
		sent_length += length;
	}
	sent_packets++;
	pthread_mutex_unlock(&sent_lock);
	return 0;
}

static int
controller_receive(uint8_t *h4 __unused, unsigned room __unused, unsigned wait_ms)
{
	usleep(wait_ms * 1000);
	return 0;
}

static const struct board_bt_controller controller_model = { "the model", controller_send, controller_receive };

/* (statics of the file under test are the test's to read) */
#include "board/bt_host.c"

/* what the controller was sent: exactly these bytes? */
static bool
sent_exactly(const uint8_t *bytes, unsigned length)
{
	bool same;

	pthread_mutex_lock(&sent_lock);
	same = sent_length == length && memcmp(sent_bytes, bytes, length) == 0;
	pthread_mutex_unlock(&sent_lock);
	return same;
}

/* an ACL packet of 5 data bytes (never answered here: it goes to the controller as it is) */
static const uint8_t acl[] = { BT_H4_ACL, 0x01, 0x20, 5, 0, 0x10, 0x11, 0x22, 0x33, 0x44 };

static void
started(void)
{
	CHECK(board_bt_start(&controller_model) == 0 && registered.ctl_connect != NULL, "started: %s", log_text);
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* bluetoothd has the socket and is half-way through a packet: a second socket is refused, and the packet arrives whole */
static void
a_second_socket(void)
{
	started();
	CHECK(connect_unit(1) == 0, "the first socket connects");
	CHECK(write_unit(1, acl, 6) == 0, "the start of a packet");
	CHECK(connect_unit(2) == EBUSY, "a second socket is refused while the first is connected");
	CHECK(logged("refused: unit 1 has it"), "said: %s", log_text);
	CHECK(write_unit(1, acl + 6, sizeof(acl) - 6) == 0, "the rest of the packet, from the first socket (still connected)");
	EVENTUALLY(sent_exactly(acl, sizeof(acl)), 2000, "the controller is sent the packet, whole (%u bytes in %u packets)",
	    sent_length, sent_packets);
	/* once the first has gone, another may have it */
	CHECK(disconnect_unit(1) == 0 && connect_unit(2) == 0, "the first gone, the second connects");
	CHECK(connect_unit(3) == EBUSY, "and a third is refused while it is there");
	CHECK(write_unit(2, acl, sizeof(acl)) == 0, "the second's packet");
	EVENTUALLY(sent_packets == 2, 2000, "it reaches the controller (%u packets)", sent_packets);
}

/* what comes from a socket that is not the connected one -- a write, the UART's settings -- is refused, and changes nothing */
static void
not_the_connected_socket(void)
{
	struct program_termios set = { .c_cflag = CS8, .c_ispeed = 115200, .c_ospeed = 115200 }, got;
	size_t length = sizeof(got);

	started();
	CHECK(write_unit(1, acl, sizeof(acl)) == ENOTCONN, "nothing connected: a write is refused");
	CHECK(connect_unit(1) == 0, "the first socket connects");
	CHECK(write_unit(2, acl, sizeof(acl)) == ENOTCONN, "a write from another unit is refused");
	CHECK(registered.ctl_setopt(&registered, 2, NULL, (int)OPTION_TIOCSETA, &set, sizeof(set)) == ENOTCONN,
	    "the UART's settings from another unit are refused");
	CHECK(registered.ctl_getopt(&registered, 2, NULL, (int)OPTION_TIOCGETA, &got, &length) == ENOTCONN && length == 0,
	    "and asking for them");
	length = sizeof(got);
	CHECK(registered.ctl_getopt(&registered, 1, NULL, (int)OPTION_TIOCGETA, &got, &length) == 0 && length == sizeof(got) &&
	    got.c_ispeed == 3000000, "the connected socket's settings are as they were (%llu baud)",
	    (unsigned long long)got.c_ispeed);
	usleep(100000);
	CHECK(sent_length == 0, "nothing reaches the controller (%u bytes)", sent_length);
	/* a refused unit's disconnect does not let the connected one go */
	CHECK(disconnect_unit(2) == 0 && write_unit(1, acl, sizeof(acl)) == 0, "the connected socket's write");
	EVENTUALLY(sent_exactly(acl, sizeof(acl)), 2000, "only it reaches the controller (%u bytes)", sent_length);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "a_second_socket", a_second_socket }, { "not_the_connected_socket", not_the_connected_socket },
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
