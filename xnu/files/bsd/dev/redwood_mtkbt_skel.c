/*
 * Bluetooth on a MediaTek phone: the skeleton. It is the part of redwood_connsys.c's Bluetooth (the Infinix Hot 20's working driver, MT6768 with
 * an MT6631 in CONNSYS; docs/bluetooth-x6826b.md) that does not depend on the phone: what iOS 16.5's bluetoothd needs from a kernel to take a
 * controller as its own -- the kernel control com.apple.uart.sk.bluetooth and the Skywalk kernel pipe behind it (redwood_btsk.c), the nodes it
 * opens, the service "bluetooth" in the registry (iokit/Kernel/RedwoodBluetooth.cpp), H4 framing of what it writes, and the answers of a
 * MediaTek controller that Apple's stack does not take as they are. What is different on every phone -- how the combo chip is powered and its
 * firmware started, how the Bluetooth function is switched on, and how a packet gets to the controller and back -- is a table of operations,
 * struct mtkbt_platform, that a port fills in. Nothing here knows a register.
 *
 * It does nothing until it is told to (boot-arg mtkbt_skel=1) AND the platform is complete: every operation that is still missing is said
 * (redwood_host_keep_add) and nothing is made. There are no defaults for what only the phone can tell.
 *
 * Not in bsd/conf/files, and not to be built together with the Bluetooth of redwood_connsys.c (both register the same kernel control and both
 * define redwood_connsys_bt_send, the name redwood_btsk.c calls). To try it: add
 *     bsd/dev/redwood_mtkbt_skel.c    standard
 * to bsd/conf/files (redwood_btsk.c and RedwoodBluetooth.cpp stay as they are), call redwood_mtkbt_skel_init() where redwood_connsys_init() is
 * called (redwood_input.c), and put your platform in mtkbt_platform below. docs/bluetooth-mtk-porting.md has the order to find things out in
 * and the user-space half, which is the same on every phone.
 *
 * Compiles with the kernel's own flags; has not run on a phone: there is no port for it to run on yet.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/sys_domain.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <sys/ttycom.h>
#include <sys/uio.h>
#include <uuid/uuid.h>

extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority, thread_t *new_thread);
extern kern_return_t thread_terminate(thread_t thread);

extern bool RedwoodBluetoothNode(void);                         /* iokit/Kernel/RedwoodBluetooth.cpp: the service "bluetooth" */
extern void RedwoodBluetoothAddress(const uint8_t *wifi);       /* its local-mac-address: the Wi-Fi address, one up */
extern void redwood_btsk_init(void);                            /* redwood_btsk.c: the Skywalk kernel pipe bluetoothd's packets go through */
extern bool redwood_btsk_uuid(uuid_t out);
extern void redwood_btsk_deliver(const uint8_t *bytes, unsigned length);
void redwood_connsys_bt_send(const uint8_t *bytes, unsigned length);    /* called by redwood_btsk.c with what bluetoothd wrote */
void redwood_mtkbt_skel_init(void);

#define say(...) redwood_host_keep_add("mtkbt-skel: " __VA_ARGS__)

#define SKEL_TXQ                8192u           /* what bluetoothd has written and the controller has not been given yet */
#define SKEL_FRAME              4096u           /* one packet from the controller */
#define SKEL_SAID_PACKETS       96u             /* of each direction, said in the log: the host buffer is 64 KB and bluetoothd's own lines want it */
#define SKEL_ANSWER_MS          2000u           /* a command of the probe is given this long */

/*
 * The operations of one phone. All but the two marked optional are required (the skeleton checks). Each returns 0 or an errno unless it says
 * otherwise, and says (redwood_host_keep_add) what it is about to do BEFORE a step that could hang a bus: the last line of a log that ends
 * is the step that hung. All of them are called from one thread, the skeleton's, and never two at once.
 */
struct mtkbt_platform {
	const char      *name;                  /* "mt6768 / x6826b / mt6631": what the log calls it */
	/*
	 * The combo chip up to the point where it takes function commands: rails, clocks, reset, the firmware patch, its start-up script (WMT on
	 * a CONNSYS SoC: redwood_connsys.c's steps 1 to 11). The same chip is Wi-Fi's: if the Wi-Fi driver has already done it, wait for it
	 * here and return 0.
	 */
	int     (*power_on)(void);
	/*
	 * Bluetooth's function on, and off. On a CONNSYS SoC it is one WMT command (WMT_FUNC_CTRL: 01 06 02 00 <0 = BT> <1 = on>, answered
	 * 02 06 01 00 00; the firmware's own events that come before the answer are not it). When function_on returns 0 the controller takes HCI.
	 */
	int     (*function_on)(void);
	void    (*function_off)(void);
	/*
	 * One whole H4 packet to the controller: the type byte (1 command, 2 ACL, 3 SCO, 5 ISO), then the HCI packet. On a combo chip behind
	 * STP that is the payload of one STP frame on channel 0 (BT_TASK_INDX).
	 */
	int     (*send)(const uint8_t *h4, unsigned length);
	/*
	 * One whole H4 packet from the controller into h4 (type byte 4 event, 2 ACL, 3 SCO, 5 ISO): its length, 0 if none came within wait_ms,
	 * less than 0 for an error. Never half a packet and never two; what is not Bluetooth's (another STP channel) is not returned.
	 */
	int     (*receive)(uint8_t *h4, unsigned room, unsigned wait_ms);
	/* optional: called before each send, for a transport that sleeps (BTIF's wake on the Infinix) */
	void    (*wake)(void);
	/*
	 * optional: the Wi-Fi radio's address, six bytes, for the registry's local-mac-address (the Bluetooth one is made of it, last byte + 1).
	 * Leave it NULL when the Wi-Fi driver calls RedwoodBluetoothAddress() itself, as redwood_net.c does. Without an address from either,
	 * bluetoothd says "No bluetooth on this device." and stops.
	 */
	int     (*wifi_address)(uint8_t out[6]);
};

/*
 * ---- THE PORT: fill this in ----
 * Nothing is assumed. For the Infinix Hot 20 each of these is a part of redwood_connsys.c: connsys_sw_init and the steps before it (power_on),
 * connsys_bt_start's WMT_FUNC_CTRL (function_on), connsys_stp_send and connsys_stp_receive on channel 0 (send, receive), connsys_btif_wake (wake).
 */
static const struct mtkbt_platform mtkbt_platform = {
	.name = NULL,
};

/* ---- the platform check ---- */
static bool
skel_platform_complete(const struct mtkbt_platform *p)
{
	bool ok = true;

#define NEED(cond, what) do { if (!(cond)) { say("the platform lacks: %s", what); ok = false; } } while (0)
	NEED(p->name != NULL, "name");
	NEED(p->power_on != NULL, "power_on (the combo chip up to where it takes function commands)");
	NEED(p->function_on != NULL && p->function_off != NULL, "function_on, function_off (Bluetooth's function)");
	NEED(p->send != NULL, "send (one H4 packet to the controller)");
	NEED(p->receive != NULL, "receive (one H4 packet from the controller)");
#undef NEED
	return ok;
}

static uint32_t
skel_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (uint32_t)(ns / NSEC_PER_MSEC);
}

/* ---- H4 ---- */

/* the length of the H4 packet that begins the bytes; 0 while its header is not whole or the first byte is not a packet type */
static unsigned
skel_h4_length(const uint8_t *b, unsigned have)
{
	if (have < 1) {
		return 0;
	}
	switch (b[0]) {
	case 1:                                 /* command: opcode (2), length (1) */
		return have >= 4 ? 4u + b[3] : 0;
	case 2:                                 /* ACL: handle (2), length (2) */
		return have >= 5 ? 5u + (b[3] | ((unsigned)b[4] << 8)) : 0;
	case 3:                                 /* SCO: handle (2), length (1) */
		return have >= 4 ? 4u + b[3] : 0;
	case 4:                                 /* event: code (1), length (1) */
		return have >= 3 ? 3u + b[2] : 0;
	case 5:                                 /* ISO: handle (2), length (2, 14 bits) */
		return have >= 5 ? 5u + ((b[3] | ((unsigned)b[4] << 8)) & 0x3fff) : 0;
	default:
		return 0;
	}
}

static void
skel_dump(const char *direction, const uint8_t *bytes, unsigned length, uint32_t *counter)
{
	char text[3 * 32 + 1];
	unsigned n = length < 32 ? length : 32;

	if ((*counter)++ >= SKEL_SAID_PACKETS) {
		return;
	}
	for (unsigned i = 0; i < n; i++) {
		snprintf(text + 3 * i, 4, " %02x", bytes[i]);
	}
	text[3 * n] = 0;
	say("%s %u bytes:%s%s", direction, length, text, length > n ? " ..." : "");
}

/* ---- what bluetoothd writes: one queue, drained a whole packet at a time by the bridge ---- */
static LCK_GRP_DECLARE(skel_lck_grp, "redwood-mtkbt-skel");
static LCK_MTX_DECLARE(skel_lock, &skel_lck_grp);
static uint8_t skel_txq[SKEL_TXQ];
static unsigned skel_txq_count;
static uint32_t skel_txq_dropped;               /* bytes that found the queue full */

static bool
skel_queue_locked(const uint8_t *bytes, size_t length)
{
	if (length > SKEL_TXQ - skel_txq_count) {
		skel_txq_dropped += (uint32_t)length;
		return false;
	}
	memcpy(skel_txq + skel_txq_count, bytes, length);
	skel_txq_count += (unsigned)length;
	return true;
}

/* H4 bytes the Skywalk channel's user wrote (redwood_btsk.c's sync_tx) */
void
redwood_connsys_bt_send(const uint8_t *bytes, unsigned length)
{
	lck_mtx_lock(&skel_lock);
	(void)skel_queue_locked(bytes, length);
	lck_mtx_unlock(&skel_lock);
}

/*
 * ---- what this controller says of itself that Apple's stack will not take as it is ----
 * Every rule here was found on the MT6631 (the runs are in redwood_connsys.c's bt_quirks and docs/bluetooth-x6826b.md). Each one only rewrites an
 * answer that has exactly the form that controller gave; a controller that answers otherwise is passed through unchanged, and then bluetoothd's
 * own log (BTLOG, src/inject.c's watcher) says which command it stopped at.
 *  - Apple's vendor commands (OGF 0x3f) are Broadcom's; the MediaTek controller says "Unknown HCI Command" (Command Status, status 1) to each
 *    and bluetoothd restarts ("VSC stall detected", reason 3602). Each is answered as done: Command Complete, status 0, and the data its parser
 *    reads after the status -- a byte for 0xfd48, 16 for 0xfce2 with sub-command 0x15 or 0x36, 64 (the first 0x51) for 0xfd1b, none for the rest
 *    (data on 0xfce9 left the next command unsent).
 *  - Write_Default_Erroneous_Data_Reporting (0x0c5b), also "Unknown HCI Command", is answered as done.
 *  - Read_Local_Supported_Commands: the controller leaves the LE commands of octets 25-28 and LE Host Support of octet 24 clear although it
 *    answers them; bluetoothd looks in the mask before it sends one and gives up ("HCI initialization failed"). The bits are OR-ed in.
 */
static uint8_t skel_vsc_sub;            /* the first parameter of the last 0xfce2 command sent: its kind */

static unsigned
skel_quirks(uint8_t *packet, unsigned length)
{
	static const uint8_t le_commands[4] = { 0xf7, 0xff, 0xff, 0x7f };

	if (length == 7 && packet[0] == 0x04 && packet[1] == 0x0f && packet[2] == 0x04 && packet[3] == 0x01 && packet[4] == 0x01 &&
	    (packet[6] >> 2) == 0x3f) {
		const bool antenna = packet[5] == 0xe2 && packet[6] == 0xfc && (skel_vsc_sub == 0x15 || skel_vsc_sub == 0x36);
		const bool cal = packet[5] == 0x1b && packet[6] == 0xfd;
		const bool data = antenna || cal || (packet[5] == 0x48 && packet[6] == 0xfd);
		const unsigned extra = cal ? 64 : (antenna ? 16 : (data ? 1 : 0));

		packet[1] = 0x0e;
		packet[2] = (uint8_t)(4 + extra);
		packet[4] = packet[5];
		packet[5] = packet[6];
		packet[6] = 0x00;
		for (unsigned i = 0; i < extra; i++) {
			packet[7 + i] = 0x00;
		}
		if (cal) {
			packet[7] = 0x51;               /* the table's number (a guess that bluetoothd took) */
		}
		return 7 + extra;
	}
	if (length == 7 && memcmp(packet, "\x04\x0f\x04\x01\x01\x5b\x0c", 7) == 0) {
		packet[1] = 0x0e;
		packet[4] = 0x5b;
		packet[5] = 0x0c;
		packet[6] = 0x00;
		return 7;
	}
	if (length >= 7 + 64 && packet[0] == 0x04 && packet[1] == 0x0e && packet[2] == 0x44 && packet[4] == 0x02 && packet[5] == 0x10 && packet[6] == 0x00) {
		for (unsigned i = 0; i < 4; i++) {
			packet[7 + 25 + i] |= le_commands[i];
		}
		packet[7 + 24] |= 0x60;
	}
	return length;
}

/*
 * ---- the kernel control ----
 * bluetoothd's transport (H4BC, the only one of its table that matches here) looks up com.apple.uart.sk.bluetooth on a PF_SYSTEM socket,
 * connects a SOCK_STREAM, and asks it (getsockopt 0x40105412, 16 bytes) for the UUID of the Skywalk channel its packets go through.
 */
static kern_ctl_ref skel_ctl_ref;

static errno_t
skel_ctl_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, __unused void **unitinfo)
{
	char name[32] = "";

	proc_selfname(name, sizeof(name));
	say("control connected by %s (pid %d): unit %u", name, proc_selfpid(), sac->sc_unit);
	return 0;
}

static errno_t
skel_ctl_disconnect(__unused kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo)
{
	say("control: unit %u disconnected", unit);
	return 0;
}

static errno_t
skel_ctl_send(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	errno_t error = 0;

	lck_mtx_lock(&skel_lock);
	for (mbuf_t part = m; part != NULL; part = mbuf_next(part)) {
		if (!skel_queue_locked(mbuf_data(part), mbuf_len(part))) {
			error = ENOBUFS;
			break;
		}
	}
	lck_mtx_unlock(&skel_lock);
	mbuf_freem(m);
	return error;
}

static errno_t
skel_ctl_setopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, __unused void *data, size_t len)
{
	say("control: setsockopt %#x, %zu bytes: taken, nothing done", (unsigned)opt, len);
	return 0;
}

static errno_t
skel_ctl_getopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t *len)
{
	uuid_t uuid;
	bool have = (unsigned)opt == 0x40105412u && data != NULL && *len >= sizeof(uuid_t) && redwood_btsk_uuid(uuid);

	say("control: getsockopt %#x, room for %zu bytes: %s", (unsigned)opt, *len, have ? "the channel's UUID" : "nothing");
	if (have) {
		memcpy(data, uuid, sizeof(uuid_t));
		*len = sizeof(uuid_t);
	} else {
		*len = 0;
	}
	return 0;
}

static bool
skel_ctl_register(void)
{
	struct kern_ctl_reg reg;
	errno_t error;

	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, "com.apple.uart.sk.bluetooth", sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;       /* (a datagram socket: "Protocol wrong type for socket") */
	reg.ctl_sendsize = 65536;
	reg.ctl_recvsize = 65536;
	reg.ctl_connect = skel_ctl_connect;
	reg.ctl_disconnect = skel_ctl_disconnect;
	reg.ctl_send = skel_ctl_send;
	reg.ctl_setopt = skel_ctl_setopt;
	reg.ctl_getopt = skel_ctl_getopt;
	error = ctl_register(&reg, &skel_ctl_ref);
	say("kernel control com.apple.uart.sk.bluetooth %s (%d)", error == 0 ? "registered" : "NOT REGISTERED", error);
	return error == 0;
}

/*
 * ---- the nodes ----
 * /dev/btwake, /dev/btreset, /dev/btpoweroff: bluetoothd opens btwake again and again, BlueTool the others; on Apple's hardware they are pins.
 * Nothing is behind them here: opened, read as empty, written and forgotten, the terminal ioctls taken. (The working driver also makes
 * /dev/cu.bluetooth and /dev/uart.bluetooth, the serial ports of the plain H4 transport; bluetoothd 16.5 does not open them, so they are not here.)
 */
static int
skel_node_open(dev_t dev, int flags, __unused int type, __unused struct proc *p)
{
	static uint32_t said;
	char name[32] = "";

	if (said < 8) {
		said++;
		proc_selfname(name, sizeof(name));
		say("node %u opened by %s (pid %d) with flags %#x", minor(dev), name, proc_selfpid(), flags);
	}
	return 0;
}

static int
skel_node_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
skel_node_read(__unused dev_t dev, __unused struct uio *uio, __unused int ioflag)
{
	return 0;
}

static int
skel_node_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	static uint32_t said;
	uint8_t first = 0;
	user_ssize_t n = uio_resid(uio);

	if (n > 0 && uiomove((caddr_t)&first, 1, uio) == 0 && said < 8) {
		said++;
		say("node %u written %lld byte(s), the first %02x: nothing is behind it", minor(dev), (long long)n, first);
	}
	uio_setresid(uio, 0);
	return 0;
}

static int
skel_node_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	static u_long said[16];
	static unsigned said_count;
	bool seen = false;
	int result = 0;

	switch (cmd) {
	case TIOCGETA: {
		struct termios *t = (struct termios *)(void *)data;

		bzero(t, sizeof(*t));
		t->c_cflag = CS8 | CREAD | CLOCAL | HUPCL;
		t->c_ispeed = t->c_ospeed = 3000000;
		break;
	}
	case TIOCSETA: case TIOCSETAW: case TIOCSETAF: case TIOCEXCL: case TIOCNXCL: case TIOCSDTR: case TIOCCDTR:
	case TIOCMBIS: case TIOCMBIC: case TIOCMSET: case TIOCFLUSH: case TIOCSBRK: case TIOCCBRK: case TIOCDRAIN:
	case FIONBIO: case FIOASYNC:
		break;
	case TIOCMGET:
		*(int *)(void *)data = TIOCM_CTS | TIOCM_DSR | TIOCM_CAR | TIOCM_RTS | TIOCM_DTR;
		break;
	case FIONREAD:
		*(int *)(void *)data = 0;
		break;
	default:
		result = ENOTTY;
		break;
	}
	for (unsigned i = 0; i < said_count; i++) {
		seen |= said[i] == cmd;
	}
	if (!seen && said_count < sizeof(said) / sizeof(said[0])) {
		said[said_count++] = cmd;
		say("node %u: ioctl %#lx: %s", minor(dev), cmd, result == 0 ? "taken" : "not a command of this node (ENOTTY)");
	}
	return result;
}

static int
skel_node_select(__unused dev_t dev, __unused int which, __unused void *wql, __unused struct proc *p)
{
	return 1;
}

static const struct cdevsw skel_cdevsw = {
	.d_open = skel_node_open, .d_close = skel_node_close, .d_read = skel_node_read, .d_write = skel_node_write,
	.d_ioctl = skel_node_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = skel_node_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static bool
skel_nodes(void)
{
	static const char *const names[] = { "btwake", "btreset", "btpoweroff" };
	int major = cdevsw_add(-1, &skel_cdevsw);
	bool made = major >= 0;

	for (unsigned i = 0; made && i < sizeof(names) / sizeof(names[0]); i++) {
		made = devfs_make_node(makedev(major, i), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "%s", names[i]) != NULL;
	}
	say("/dev/btwake, /dev/btreset, /dev/btpoweroff %s (major %d)", made ? "made" : "NOT MADE", major);
	return made;
}

/* ---- the controller ---- */
static uint8_t skel_frame[SKEL_FRAME];
static uint8_t skel_packet[SKEL_TXQ];
static uint32_t skel_said_in, skel_said_out;

/*
 * One packet from the controller, checked: its length is the one its own header says. Anything else is not published (the stack above would
 * lose its place in the stream) and is said, the first few times. 0 if none, less than 0 for the platform's error.
 */
static int
skel_receive(unsigned wait_ms)
{
	static uint32_t said_bad;
	int length = mtkbt_platform.receive(skel_frame, SKEL_FRAME - 64, wait_ms);     /* (64: room for what skel_quirks adds) */

	if (length <= 0) {
		return length;
	}
	if ((unsigned)length > SKEL_FRAME - 64 || skel_h4_length(skel_frame, (unsigned)length) != (unsigned)length) {
		if (said_bad < 8) {
			said_bad++;
			say("receive gave %d bytes that are not one H4 packet (type %02x, its header says %u): dropped", length, skel_frame[0],
			    (unsigned)length <= SKEL_FRAME - 64 ? skel_h4_length(skel_frame, (unsigned)length) : 0);
		}
		return 0;
	}
	return length;
}

/* A command of the probe: sent, and answered by a Command Complete of its opcode with status 0 within SKEL_ANSWER_MS. */
static bool
skel_hci(const char *what, uint16_t opcode)
{
	const struct mtkbt_platform *p = &mtkbt_platform;
	const uint8_t command[4] = { 0x01, (uint8_t)opcode, (uint8_t)(opcode >> 8), 0x00 };
	uint32_t since = skel_ms();
	int error, length;

	if (p->wake != NULL) {
		p->wake();
	}
	skel_dump("out", command, sizeof(command), &skel_said_out);
	if ((error = p->send(command, sizeof(command))) != 0) {
		say("%s: send failed (%d)", what, error);
		return false;
	}
	while (skel_ms() - since < SKEL_ANSWER_MS) {
		if ((length = skel_receive(10)) < 0) {
			say("%s: receive failed (%d)", what, length);
			return false;
		}
		if (length == 0) {
			continue;
		}
		skel_dump("in ", skel_frame, (unsigned)length, &skel_said_in);
		if (length >= 7 && skel_frame[0] == 0x04 && skel_frame[1] == 0x0e && skel_frame[4] == command[1] && skel_frame[5] == command[2]) {
			if (skel_frame[6] != 0) {
				say("%s: the controller's status is %#x", what, skel_frame[6]);
			}
			return skel_frame[6] == 0;
		}
	}
	say("%s: no Command Complete in %u ms", what, SKEL_ANSWER_MS);
	return false;
}

/* what is written is sent a packet at a time; what comes is given to the Skywalk channel. Never returns. */
static void
skel_bridge(void)
{
	const struct mtkbt_platform *p = &mtkbt_platform;
	uint32_t pending_since = 0, errors = 0;
	bool said_pending = false;

	for (;;) {
		int length;

		for (;;) {
			unsigned n, have;
			int error;

			lck_mtx_lock(&skel_lock);
			have = skel_txq_count;
			if (have > 0 && (skel_txq[0] < 1 || skel_txq[0] > 5 || skel_txq[0] == 4)) {
				/* not a packet the host sends: the stream has lost its framing and nothing after it can be trusted */
				say("%u bytes written begin with %02x, not an H4 packet type: all dropped", have, skel_txq[0]);
				skel_txq_count = 0;
				lck_mtx_unlock(&skel_lock);
				break;
			}
			n = skel_h4_length(skel_txq, have);
			if (n == 0 || n > have) {
				if (n > SKEL_TXQ) {
					say("a packet of %u bytes is longer than the queue (%u): %u bytes dropped", n, SKEL_TXQ, have);
					skel_txq_count = 0;
				}
				lck_mtx_unlock(&skel_lock);
				break;
			}
			memcpy(skel_packet, skel_txq, n);
			memmove(skel_txq, skel_txq + n, have - n);
			skel_txq_count = have - n;
			lck_mtx_unlock(&skel_lock);
			if (n >= 5 && skel_packet[0] == 1 && skel_packet[1] == 0xe2 && skel_packet[2] == 0xfc) {
				skel_vsc_sub = skel_packet[4];
			}
			skel_dump("out", skel_packet, n, &skel_said_out);
			if (p->wake != NULL) {
				p->wake();
			}
			if ((error = p->send(skel_packet, n)) != 0 && errors++ < 8) {
				say("send of %u bytes failed (%d)", n, error);
			}
		}
		{
			/* bytes written and not a whole packet for 2 s: said once (a writer that stopped half-way, or a length this cannot frame) */
			unsigned pending;
			uint32_t dropped;

			lck_mtx_lock(&skel_lock);
			pending = skel_txq_count;
			dropped = skel_txq_dropped;
			skel_txq_dropped = 0;
			lck_mtx_unlock(&skel_lock);
			if (dropped != 0) {
				say("the queue was full: %u bytes from bluetoothd were dropped", dropped);
			}
			if (pending == 0) {
				pending_since = 0;
			} else if (pending_since == 0) {
				pending_since = skel_ms() | 1;
			} else if (!said_pending && skel_ms() - pending_since > 2000) {
				said_pending = true;
				say("%u bytes written and not a whole H4 packet in 2 s", pending);
			}
		}
		length = skel_receive(2);
		if (length > 0) {
			length = (int)skel_quirks(skel_frame, (unsigned)length);
			skel_dump("in ", skel_frame, (unsigned)length, &skel_said_in);
			redwood_btsk_deliver(skel_frame, (unsigned)length);
		} else if (length < 0) {
			if (errors++ < 8) {
				say("receive failed (%d)", length);
			}
			delay_for_interval(1, NSEC_PER_MSEC);
		}
	}
}

static void
skel_thread(__unused void *arg, __unused wait_result_t w)
{
	static const struct {
		const char *what;
		uint16_t opcode;
	} probe[] = {
		{ "HCI_Reset", 0x0c03 },
		{ "HCI Read_Local_Version_Information", 0x1001 },
		{ "HCI Read_BD_ADDR", 0x1009 },
		{ "HCI Read_Local_Supported_Features", 0x1003 },
		{ "HCI Read_Buffer_Size", 0x1005 },
		{ "HCI LE_Read_Buffer_Size", 0x2002 },
	};
	const struct mtkbt_platform *p = &mtkbt_platform;
	unsigned good = 0;
	int error;

	if (!skel_platform_complete(p)) {
		say("the platform is not complete: no Bluetooth");
		goto out;
	}
	say("%s: power on", p->name);
	if ((error = p->power_on()) != 0) {
		say("power_on failed (%d): no Bluetooth", error);
		goto out;
	}
	say("%s: Bluetooth's function on", p->name);
	if ((error = p->function_on()) != 0) {
		say("function_on failed (%d): no Bluetooth", error);
		goto out;
	}
	/* the controller, asked what a host asks first; all six must be answered before iOS is shown anything */
	for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
		good += skel_hci(probe[i].what, probe[i].opcode);
	}
	say("%u of %u HCI commands answered as a controller answers", good, (unsigned)(sizeof(probe) / sizeof(probe[0])));
	if (good != sizeof(probe) / sizeof(probe[0])) {
		say("not a controller iOS can be given: Bluetooth's function off");
		p->function_off();
		goto out;
	}
	if (p->wifi_address != NULL) {
		uint8_t wifi[6] = { 0 };
		uint8_t all_or = 0, all_and = 0xff;

		error = p->wifi_address(wifi);
		for (unsigned i = 0; i < 6; i++) {
			all_or |= wifi[i];
			all_and &= wifi[i];
		}
		if (error != 0 || all_or == 0 || all_and == 0xff || (wifi[0] & 1)) {
			say("wifi_address gave no address of a station (%d, %02x:%02x:%02x:%02x:%02x:%02x): none is published", error,
			    wifi[0], wifi[1], wifi[2], wifi[3], wifi[4], wifi[5]);
		} else {
			RedwoodBluetoothAddress(wifi);
		}
	}
	/* iOS looks for the controller in the registry ("bluetooth"): without it BluetoothManager's attach fails with 9 and Settings' switch stays grey */
	{
		bool node = false;

		for (unsigned tries = 0; tries < 40 && !(node = RedwoodBluetoothNode()); tries++) {
			delay_for_interval(250, NSEC_PER_MSEC);
		}
		if (!node) {
			say("the registry's \"bluetooth\" service could not be made in 10 s: Settings will not attach");
		}
	}
	if (!skel_nodes() || !skel_ctl_register()) {
		p->function_off();
		goto out;
	}
	redwood_btsk_init();
	say("%s: the bridge runs", p->name);
	skel_bridge();
out:
	thread_terminate(current_thread());
}

void
redwood_mtkbt_skel_init(void)
{
	uint32_t level = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("mtkbt_skel", &level, sizeof(level)) || level < 1) {
		return;
	}
	if (kernel_thread_start_priority(skel_thread, NULL, 81 /* BASEPRI_KERNEL */, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
