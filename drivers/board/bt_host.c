/*
 * Bluetooth: the side bluetoothd talks to, for a controller a driver of ours has brought up (include/board/bt.h).
 *
 * What bluetoothd opens, without BT_HCI_TRANSPORT in its environment (Darwin 27's as 16.7's):
 *   - the IORegistry service "bluetooth" (iokit/Kernel/BoardBluetooth.cpp): the address, and transport-encoding 3,
 *     "H4BC";
 *   - for H4BC, the kernel control com.apple.uart.sk.bluetooth (PF_SYSTEM, SOCK_STREAM). On it: getsockopt
 *     0x40105412 gives the UUID of a Skywalk channel, which it opens (os_channel_create) and through which the
 *     packets go, whole H4 packets a slot each; then the UART's settings on the same socket, as if it were one
 *     (TIOCGETA, cfmakeraw, a speed, TIOCSETA: kept and given back);
 *   - /dev/btwake, /dev/btreset and /dev/btpoweroff, which it opens and writes to (nothing is behind them here).
 * A thread of ours carries the packets: what bluetoothd writes, a whole H4 packet at a time, to the controller --
 * or answered here instead (board/bt_hci.h: what a controller that is not Broadcom's or Apple's does not answer as
 * bluetoothd wants) -- and what the controller says, as bluetoothd is to see it, into the channel.
 *
 * After HakiOS's MediaTek port (hakios-mt6768: redwood_btsk.c, redwood_mtkbt_skel.c) and the OnePlus Nord 2 port of
 * iOS built on it (frxncyyy/ios-denniz, piece 0721), both Apple Public Source License 2.0, as is this file.
 *
 * bt_quirks=BITS: what is answered or changed (board/bt_hci.h, default BT_Q_DEFAULT).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/random.h>
#include <sys/systm.h>
#include <sys/ttycom.h>
#include <sys/uio.h>
#include <sys/board/bt.h>
#include <skywalk/os_skywalk_private.h>

#include "bt_hci.h"
#include "bt_queue.h"
#include "log.h"

HAKIOS_DEFINE_SAY(ml_get_interrupts_enabled())

static const struct board_bt_controller *controller;
static uint32_t quirks = BT_Q_DEFAULT;

static LCK_GRP_DECLARE(bt_host_lck_grp, "board-bt");
static LCK_MTX_DECLARE(bt_host_lock, &bt_host_lck_grp);

/* what bluetoothd wrote, not sent yet (a line: whole packets, perhaps the start of one more), and the whole packets for
 * bluetoothd not in the channel yet (a ring): board/bt_queue.h, under bt_host_lock */
static uint8_t tx_bytes[16384], rx_bytes[32768];
static struct bt_line tx_line = { tx_bytes, sizeof(tx_bytes), 0 };
static struct bt_ring rx_ring_queue = { rx_bytes, sizeof(rx_bytes), 0, 0 };
static uint32_t tx_dropped, rx_dropped, answered_here, sent, received;

/* ---- the Skywalk pipe -------------------------------------------------------------------------------------- */

static nexus_controller_t pipe_controller;
static uuid_t pipe_domain, pipe_provider, pipe_instance;
static bool pipe_ready;
static uintptr_t pipe_context;
static kern_channel_ring_t tx_ring, rx_ring;
static kern_pbufpool_t tx_pool, rx_pool;
static bool channel_open;                       /* bluetoothd has the channel: what the controller says is queued for it */
static bool tx_waiting;                         /* a packet of bluetoothd's is left in the TX ring for want of room */

static errno_t pipe_domain_init(__unused kern_nexus_domain_provider_t d) { return 0; }
static void pipe_domain_fini(__unused kern_nexus_domain_provider_t d) { }

static errno_t
pipe_pre_connect(__unused kern_nexus_provider_t p, proc_t who, kern_nexus_t nexus, __unused nexus_port_t port, kern_channel_t channel,
    void **context)
{
	errno_t error = kern_nexus_get_pbufpool(nexus, &tx_pool, &rx_pool);

	*context = channel;
	say("bt: the channel opened by pid %d (%d)\n", proc_pid(who), error);
	/* (without its pools a TX packet could not be given back, nor an RX one made: the channel is refused) */
	return error != 0 ? error : (tx_pool == NULL || rx_pool == NULL) ? ENOMEM : 0;
}

/* a channel opened: a session of bluetoothd's begins, with nothing of the last one's (what the controller said for it, the
 * start of a packet it did not finish) */
static errno_t
pipe_connected(__unused kern_nexus_provider_t p, __unused kern_nexus_t n, __unused kern_channel_t c)
{
	lck_mtx_lock(&bt_host_lock);
	rx_ring_queue.head = rx_ring_queue.count = 0;
	tx_line.count = 0;
	channel_open = true;
	lck_mtx_unlock(&bt_host_lock);
	return 0;
}
static void pipe_pre_disconnect(__unused kern_nexus_provider_t p, __unused kern_nexus_t n, __unused kern_channel_t c) { }

static void
pipe_disconnected(__unused kern_nexus_provider_t p, __unused kern_nexus_t n, __unused kern_channel_t c)
{
	lck_mtx_lock(&bt_host_lock);
	channel_open = false;
	lck_mtx_unlock(&bt_host_lock);
	say("bt: the channel closed\n");
}

static errno_t
pipe_ring_init(__unused kern_nexus_provider_t p, __unused kern_nexus_t n, __unused kern_channel_t c, kern_channel_ring_t ring,
    boolean_t is_tx, void **context)
{
	lck_mtx_lock(&bt_host_lock);
	if (is_tx) {
		tx_ring = ring;
	} else {
		rx_ring = ring;
	}
	lck_mtx_unlock(&bt_host_lock);
	*context = ring;
	return 0;
}

static void
pipe_ring_fini(__unused kern_nexus_provider_t p, __unused kern_nexus_t n, kern_channel_ring_t ring)
{
	lck_mtx_lock(&bt_host_lock);
	if (ring == tx_ring) {
		tx_ring = NULL;
	} else if (ring == rx_ring) {
		rx_ring = NULL;
	}
	lck_mtx_unlock(&bt_host_lock);
}

static errno_t
pipe_slot_init(__unused kern_nexus_provider_t p, __unused kern_nexus_t n, __unused kern_channel_ring_t r, channel_slot_t slot,
    __unused uint32_t index, struct kern_slot_prop **properties, void **context)
{
	*properties = NULL;
	*context = slot;
	return 0;
}

static void pipe_slot_fini(__unused kern_nexus_provider_t p, __unused kern_nexus_t n, __unused kern_channel_ring_t r,
    __unused channel_slot_t s, __unused uint32_t index) { }

/* bytes bluetoothd wrote into the line for the controller; false when there is no room for them (they are dropped) */
static bool
tx_put_locked(const uint8_t *bytes, size_t length)
{
	if (length > tx_line.size || !bt_line_put(&tx_line, bytes, (unsigned)length)) {
		tx_dropped += (uint32_t)length;
		return false;
	}
	return true;
}

/*
 * What bluetoothd put into the channel's TX ring: into the line, its slots given back -- as far as the line has room. A
 * packet it has no room for yet is left in its slot, and those after it, in their order: the channel is told once the
 * line has drained (carry), and bluetoothd's next sync takes them. (One longer than the whole line could never go: it is
 * dropped.) A command or an ACL packet thrown away instead would leave bluetoothd waiting for an answer, or for the
 * credit of a packet, that never comes.
 */
static errno_t
pipe_sync_tx(__unused kern_nexus_provider_t p, __unused kern_nexus_t n, kern_channel_ring_t ring, __unused uint32_t flags)
{
	kern_channel_slot_t slot = kern_channel_get_next_slot(ring, NULL, NULL), last = NULL;

	while (slot != NULL) {
		kern_packet_t packet = kern_channel_slot_get_packet(ring, slot);

		if (packet != 0) {
			kern_buflet_t buflet = kern_packet_get_next_buflet(packet, NULL);
			uint8_t *base = buflet != NULL ? kern_buflet_get_data_address(buflet) : NULL;
			uint32_t length = base != NULL ? kern_buflet_get_data_length(buflet) : 0;
			bool room = true;

			if (length > 0) {
				lck_mtx_lock(&bt_host_lock);
				if (length <= tx_line.size && length > tx_line.size - tx_line.count) {
					room = false;
					tx_waiting = true;
				} else {
					(void)tx_put_locked(base + kern_buflet_get_data_offset(buflet), length);
				}
				lck_mtx_unlock(&bt_host_lock);
			}
			if (!room) {
				break;
			}
			(void)kern_channel_slot_detach_packet(ring, slot, packet);
			if (tx_pool != NULL) {
				kern_pbufpool_free(tx_pool, packet);
			}
		}
		last = slot;
		slot = kern_channel_get_next_slot(ring, slot, NULL);
	}
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
	}
	(void)kern_channel_reclaim(ring);
	return 0;
}

/* the packets for bluetoothd into the RX ring's free slots, a packet a slot */
static errno_t
pipe_sync_rx(__unused kern_nexus_provider_t p, __unused kern_nexus_t n, kern_channel_ring_t ring, __unused uint32_t flags)
{
	kern_channel_slot_t slot, last = NULL;

	(void)kern_channel_reclaim(ring);
	slot = kern_channel_get_next_slot(ring, NULL, NULL);
	lck_mtx_lock(&bt_host_lock);
	while (slot != NULL && rx_ring_queue.count > 0 && rx_pool != NULL) {
		kern_packet_t packet = 0;
		kern_buflet_t buflet;
		uint8_t *base;
		unsigned length = bt_ring_next(&rx_ring_queue);

		if (length == 0) {
			(void)bt_ring_take(&rx_ring_queue, NULL);      /* (cannot be: only whole packets are queued) */
			break;
		}
		if (kern_pbufpool_alloc_nosleep(rx_pool, 1, &packet) != 0 || packet == 0) {
			break;
		}
		buflet = kern_packet_get_next_buflet(packet, NULL);
		base = buflet != NULL ? kern_buflet_get_data_address(buflet) : NULL;
		if (base == NULL || kern_buflet_get_data_limit(buflet) < length) {
			kern_pbufpool_free(rx_pool, packet);
			(void)bt_ring_take(&rx_ring_queue, NULL);      /* (one that does not fit a slot: dropped) */
			rx_dropped++;
			continue;
		}
		(void)bt_ring_take(&rx_ring_queue, base);
		(void)kern_buflet_set_data_offset(buflet, 0);
		(void)kern_buflet_set_data_length(buflet, (uint16_t)length);
		(void)kern_packet_finalize(packet);
		if (kern_channel_slot_attach_packet(ring, slot, packet) != 0) {
			kern_pbufpool_free(rx_pool, packet);
			break;
		}
		last = slot;
		slot = kern_channel_get_next_slot(ring, slot, NULL);
	}
	lck_mtx_unlock(&bt_host_lock);
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
	}
	return 0;
}

/*
 * A whole packet for bluetoothd: queued while it has the channel (else dropped: a session that has not begun is not to
 * get it), and the channel told. Told under the lock: pipe_ring_fini takes it before the ring goes away, so the ring is
 * not one a closing channel has freed; and telling only wakes whoever waits on the channel (na_notify), never coming
 * back into pipe_sync_rx.
 */
static void
deliver(const uint8_t *packet, unsigned length)
{
	lck_mtx_lock(&bt_host_lock);
	if (!channel_open || !bt_ring_put(&rx_ring_queue, packet, length)) {
		rx_dropped++;
	} else if (rx_ring != NULL) {
		(void)kern_channel_notify(rx_ring, 0);
	}
	lck_mtx_unlock(&bt_host_lock);
}

/* what pipe_make made, taken away again, the last first (what it did not get to is null) */
static void
pipe_unmake(void)
{
	lck_mtx_lock(&bt_host_lock);
	pipe_ready = false;
	lck_mtx_unlock(&bt_host_lock);
	if (pipe_controller != NULL) {
		if (!uuid_is_null(pipe_instance)) {
			(void)kern_nexus_controller_free_provider_instance(pipe_controller, pipe_instance);
			uuid_clear(pipe_instance);
		}
		if (!uuid_is_null(pipe_provider)) {
			(void)kern_nexus_controller_deregister_provider(pipe_controller, pipe_provider);
			uuid_clear(pipe_provider);
		}
		kern_nexus_controller_destroy(pipe_controller);
		pipe_controller = NULL;
	}
	if (!uuid_is_null(pipe_domain)) {
		(void)kern_nexus_deregister_domain_provider(pipe_domain);
		uuid_clear(pipe_domain);
	}
}

static int
pipe_make(void)
{
	nexus_attr_t attributes = NULL;
	errno_t error;
	struct kern_nexus_domain_provider_init domain = {
		.nxdpi_version = KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION, .nxdpi_flags = 0,
		.nxdpi_init = pipe_domain_init, .nxdpi_fini = pipe_domain_fini,
	};
	struct kern_nexus_provider_init provider = {
		.nxpi_version = KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION, .nxpi_flags = NXPIF_VIRTUAL_DEVICE,
		.nxpi_pre_connect = pipe_pre_connect, .nxpi_connected = pipe_connected, .nxpi_pre_disconnect = pipe_pre_disconnect,
		.nxpi_disconnected = pipe_disconnected, .nxpi_ring_init = pipe_ring_init, .nxpi_ring_fini = pipe_ring_fini,
		.nxpi_slot_init = pipe_slot_init, .nxpi_slot_fini = pipe_slot_fini, .nxpi_sync_tx = pipe_sync_tx,
		.nxpi_sync_rx = pipe_sync_rx, .nxpi_tx_doorbell = NULL,
	};
	struct kern_nexus_init instance = { .nxi_version = KERN_NEXUS_CURRENT_VERSION, .nxi_flags = 0 };

	read_random(&pipe_context, sizeof(pipe_context));
	if ((error = kern_nexus_register_domain_provider(NEXUS_TYPE_KERNEL_PIPE, (const uint8_t *)"board_bt", &domain, sizeof(domain),
	    &pipe_domain)) != 0 || (error = kern_nexus_controller_create(&pipe_controller)) != 0 ||
	    (error = kern_nexus_attr_create(&attributes)) != 0) {
		say("bt: the Skywalk pipe cannot be made (%d)\n", error);
		pipe_unmake();
		return error;
	}
	(void)kern_nexus_attr_set(attributes, NEXUS_ATTR_ANONYMOUS, 1);
	(void)kern_nexus_attr_set(attributes, NEXUS_ATTR_TX_RINGS, 1);
	(void)kern_nexus_attr_set(attributes, NEXUS_ATTR_RX_RINGS, 1);
	(void)kern_nexus_attr_set(attributes, NEXUS_ATTR_TX_SLOTS, 64);
	(void)kern_nexus_attr_set(attributes, NEXUS_ATTR_RX_SLOTS, 128);
	(void)kern_nexus_attr_set(attributes, NEXUS_ATTR_SLOT_BUF_SIZE, 2048);
	error = kern_nexus_controller_register_provider(pipe_controller, pipe_domain, (const uint8_t *)"com.apple.nexus.board_bt",
	    &provider, sizeof(provider), attributes, &pipe_provider);
	kern_nexus_attr_destroy(attributes);
	if (error == 0) {
		error = kern_nexus_controller_alloc_provider_instance(pipe_controller, pipe_provider, (const void *)pipe_context, NULL,
		    &pipe_instance, &instance);
	}
	if (error != 0) {
		say("bt: the Skywalk pipe cannot be made (%d)\n", error);
		pipe_unmake();
		return error;
	}
	lck_mtx_lock(&bt_host_lock);
	pipe_ready = true;
	lck_mtx_unlock(&bt_host_lock);
	return 0;
}

/* ---- the kernel control ------------------------------------------------------------------------------------ */

/* struct termios of an arm64 program: its flags and speeds 64 bits each, 20 control characters */
struct program_termios {
	uint64_t c_iflag, c_oflag, c_cflag, c_lflag;
	uint8_t c_cc[20];
	uint8_t padding[4];
	uint64_t c_ispeed, c_ospeed;
};
_Static_assert(sizeof(struct program_termios) == 72, "an arm64 program's termios");

#define OPTION_CHANNEL_UUID     0x40105412u     /* getsockopt: the Skywalk channel's UUID */
#define OPTION_TIOCGETA         0x40487413u
#define OPTION_TIOCSETA         0x80487414u
#define OPTION_TIOCSETAW        0x80487415u
#define OPTION_TIOCSETAF        0x80487416u

static struct program_termios termios = { .c_cflag = CS8 | CREAD | CLOCAL, .c_ispeed = 3000000, .c_ospeed = 3000000 };
static kern_ctl_ref control;

/*
 * One socket at a time, the unit of the one connected (under bt_host_lock): there is one line, one UART's settings, and a
 * second socket's connect would have emptied the line under the first one's half-written packet, its writes run into
 * the first one's, its settings taken for the UART's. Another is refused while one is connected (EBUSY), and what is
 * not the connected one's is refused too.
 */
static bool client_connected;
static u_int32_t client_unit;

/* (under bt_host_lock) */
static bool
is_client_locked(u_int32_t unit)
{
	return client_connected && client_unit == unit;
}

static errno_t
control_connect(__unused kern_ctl_ref r, struct sockaddr_ctl *address, __unused void **context)
{
	char name[32] = "";
	u_int32_t had = 0;
	bool busy;

	lck_mtx_lock(&bt_host_lock);
	if (!(busy = client_connected)) {
		client_connected = true;
		client_unit = address->sc_unit;
		tx_line.count = 0;              /* (nothing of a socket before this one's) */
	} else {
		had = client_unit;
	}
	lck_mtx_unlock(&bt_host_lock);
	proc_selfname(name, sizeof(name));
	if (busy) {
		say("bt: com.apple.uart.sk.bluetooth: %s (pid %d, unit %u) refused: unit %u has it\n", name, proc_selfpid(),
		    address->sc_unit, had);
		return EBUSY;
	}
	say("bt: com.apple.uart.sk.bluetooth connected by %s (pid %d, unit %u)\n", name, proc_selfpid(), address->sc_unit);
	return 0;
}

/* (also called for a socket whose connect was refused, with its own unit: the connected one is let go only by its own) */
static errno_t
control_disconnect(__unused kern_ctl_ref r, u_int32_t unit, __unused void *context)
{
	bool was;

	lck_mtx_lock(&bt_host_lock);
	if ((was = is_client_locked(unit))) {
		client_connected = false;
	}
	lck_mtx_unlock(&bt_host_lock);
	if (was) {
		say("bt: com.apple.uart.sk.bluetooth unit %u disconnected (%u sent, %u received, %u answered here)\n", unit, sent,
		    received, answered_here);
	}
	return 0;
}

/* (written to the socket itself: the same as into the channel) -- all of a write or none of it: the start of a packet
 * left in the line would have the next one's bytes taken for the rest of it */
static errno_t
control_send(__unused kern_ctl_ref r, u_int32_t unit, __unused void *context, mbuf_t m, __unused int flags)
{
	errno_t error = 0;
	size_t total = 0;

	for (mbuf_t part = m; part != NULL; part = mbuf_next(part)) {
		total += mbuf_len(part);
	}
	lck_mtx_lock(&bt_host_lock);
	if (!is_client_locked(unit)) {
		error = ENOTCONN;
	} else if (total > tx_line.size - tx_line.count) {
		tx_dropped += (uint32_t)total;
		error = ENOBUFS;
	} else {
		for (mbuf_t part = m; part != NULL; part = mbuf_next(part)) {
			(void)bt_line_put(&tx_line, mbuf_data(part), (unsigned)mbuf_len(part));
		}
	}
	lck_mtx_unlock(&bt_host_lock);
	mbuf_freem(m);
	return error;
}

static errno_t
control_setopt(__unused kern_ctl_ref r, u_int32_t unit, __unused void *context, int option, void *data, size_t length)
{
	uint32_t o = (uint32_t)option;
	errno_t error = 0;

	lck_mtx_lock(&bt_host_lock);
	if (!is_client_locked(unit)) {
		error = ENOTCONN;
	} else if ((o == OPTION_TIOCSETA || o == OPTION_TIOCSETAW || o == OPTION_TIOCSETAF) && data != NULL &&
	    length == sizeof(termios)) {
		memcpy(&termios, data, sizeof(termios));        /* (kept, and given back by TIOCGETA) */
	}
	lck_mtx_unlock(&bt_host_lock);
	return error;
}

static errno_t
control_getopt(__unused kern_ctl_ref r, u_int32_t unit, __unused void *context, int option, void *data, size_t *length)
{
	uint32_t o = (uint32_t)option;
	errno_t error = 0;

	lck_mtx_lock(&bt_host_lock);
	if (!is_client_locked(unit)) {
		error = ENOTCONN;
		*length = 0;
	} else if (o == OPTION_CHANNEL_UUID && data != NULL && *length >= sizeof(uuid_t) && pipe_ready) {
		memcpy(data, pipe_instance, sizeof(uuid_t));
		*length = sizeof(uuid_t);
	} else if (o == OPTION_TIOCGETA && data != NULL && *length >= sizeof(termios)) {
		memcpy(data, &termios, sizeof(termios));
		*length = sizeof(termios);
	} else {
		*length = 0;
	}
	lck_mtx_unlock(&bt_host_lock);
	return error;
}

static int
control_register(void)
{
	struct kern_ctl_reg reg;
	errno_t error;

	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, "com.apple.uart.sk.bluetooth", sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	reg.ctl_sendsize = 65536;
	reg.ctl_recvsize = 65536;
	reg.ctl_connect = control_connect;
	reg.ctl_disconnect = control_disconnect;
	reg.ctl_send = control_send;
	reg.ctl_setopt = control_setopt;
	reg.ctl_getopt = control_getopt;
	if ((error = ctl_register(&reg, &control)) != 0) {
		say("bt: the kernel control com.apple.uart.sk.bluetooth cannot be registered (%d)\n", error);
		control = NULL;
	}
	return error;
}

/* ---- /dev/btwake, /dev/btreset, /dev/btpoweroff: opened and written to by bluetoothd; nothing behind them --------- */

static int node_open(__unused dev_t d, __unused int f, __unused int t, __unused struct proc *p) { return 0; }
static int node_close(__unused dev_t d, __unused int f, __unused int t, __unused struct proc *p) { return 0; }
static int node_read(__unused dev_t d, __unused struct uio *u, __unused int f) { return 0; }
static const char *const node_names[] = { "btwake", "btreset", "btpoweroff" };

/* what bluetoothd writes is taken, and nothing done with it -- but said, the first times: what it asks of these nodes is
 * for a phone's log to show (a reset, the power, the wake line), before anything is put behind them */
static int
node_write(dev_t dev, struct uio *uio, __unused int f)
{
	static unsigned said;
	uint8_t bytes[8];
	user_ssize_t length = uio_resid(uio);
	int error = 0;

	if (length > 0 && said < 40) {
		size_t n = length < (user_ssize_t)sizeof(bytes) ? (size_t)length : sizeof(bytes);

		if ((error = uiomove((caddr_t)bytes, (int)n, uio)) == 0) {
			said++;
			say("bt: bluetoothd wrote %lld bytes to /dev/%s, the first %02x (nothing is behind it)\n", (long long)length,
			    minor(dev) < 3 ? node_names[minor(dev)] : "?", bytes[0]);
		}
	}
	uio_setresid(uio, 0);
	return error;
}
static int node_select(__unused dev_t d, __unused int w, __unused void *wql, __unused struct proc *p) { return 1; }

static int
node_ioctl(__unused dev_t dev, u_long command, caddr_t data, __unused int flag, __unused struct proc *p)
{
	switch (command) {
	case TIOCGETA: {
		struct termios *t = (struct termios *)(void *)data;

		bzero(t, sizeof(*t));
		t->c_cflag = CS8 | CREAD | CLOCAL | HUPCL;
		t->c_ispeed = t->c_ospeed = 3000000;
		return 0;
	}
	case TIOCSETA: case TIOCSETAW: case TIOCSETAF: case TIOCEXCL: case TIOCNXCL: case TIOCSDTR: case TIOCCDTR:
	case TIOCMBIS: case TIOCMBIC: case TIOCMSET: case TIOCFLUSH: case TIOCSBRK: case TIOCCBRK: case TIOCDRAIN:
	case FIONBIO: case FIOASYNC:
		return 0;
	case TIOCMGET:
		*(int *)(void *)data = TIOCM_CTS | TIOCM_DSR | TIOCM_CAR | TIOCM_RTS | TIOCM_DTR;
		return 0;
	case FIONREAD:
		*(int *)(void *)data = 0;
		return 0;
	default:
		return ENOTTY;
	}
}

static const struct cdevsw node_cdevsw = {
	.d_open = node_open, .d_close = node_close, .d_read = node_read, .d_write = node_write, .d_ioctl = node_ioctl,
	.d_stop = eno_stop, .d_reset = eno_reset, .d_select = node_select, .d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static int node_major = -1;
static void *node_handles[3];

static void
nodes_remove(void)
{
	for (unsigned i = 0; i < 3; i++) {
		if (node_handles[i] != NULL) {
			devfs_remove(node_handles[i]);
			node_handles[i] = NULL;
		}
	}
	if (node_major >= 0) {
		(void)cdevsw_remove(node_major, &node_cdevsw);
		node_major = -1;
	}
}

static int
nodes_make(void)
{
	if ((node_major = cdevsw_add(-1, &node_cdevsw)) < 0) {
		say("bt: /dev/btwake and its fellows cannot be made\n");
		return ENXIO;
	}
	for (unsigned i = 0; i < 3; i++) {
		node_handles[i] = devfs_make_node(makedev(node_major, i), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "%s", node_names[i]);
		if (node_handles[i] == NULL) {
			say("bt: /dev/%s cannot be made\n", node_names[i]);
			nodes_remove();
			return ENXIO;
		}
	}
	return 0;
}

/* ---- the thread that carries the packets --------------------------------------------------------------------- */

static uint8_t to_controller[BT_H4_MOST], from_controller[BT_H4_MOST], answer[300];

static void
carry(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		int length;

		/* what bluetoothd wrote: whole packets, to the controller or answered here */
		for (;;) {
			unsigned have;
			uint8_t first;
			int n;

			lck_mtx_lock(&bt_host_lock);
			have = tx_line.count;
			first = tx_bytes[0];
			n = bt_line_take(&tx_line, to_controller, sizeof(to_controller), &tx_dropped);
			if (n != 0 && tx_waiting && tx_ring != NULL) {
				tx_waiting = false;             /* room again: bluetoothd is told, and its next sync takes what waits */
				(void)kern_channel_notify(tx_ring, 0);
			}
			lck_mtx_unlock(&bt_host_lock);
			if (n < 0) {
				say("bt: %u bytes from bluetoothd that begin with %#x, no H4 packet a host sends: dropped\n", have, first);
			}
			if (n <= 0) {
				break;
			}
			if ((length = (int)bt_answer_here(to_controller, (unsigned)n, quirks, answer, sizeof(answer))) > 0) {
				answered_here++;
				deliver(answer, (unsigned)length);
			} else if (controller->send(to_controller, (unsigned)n) == 0) {
				sent++;
			} else {
				lck_mtx_lock(&bt_host_lock);
				tx_dropped += (unsigned)n;
				lck_mtx_unlock(&bt_host_lock);
			}
		}
		/* what the controller says, a few milliseconds' wait at most and a few packets at most, so that bluetoothd's
		 * packets are not kept waiting: an LE scan in a busy place has the controller send reports back to back, and a
		 * command to stop it would wait behind them */
		for (unsigned k = 0; k < 8 && (length = controller->receive(from_controller, sizeof(from_controller), 5)) > 0; k++) {
			received++;
			if ((length = (int)bt_rewrite(from_controller, (unsigned)length, quirks)) > 0) {
				deliver(from_controller, (unsigned)length);
			}
		}
	}
}

/*
 * The Darwin side, made in the order bluetoothd comes to it backwards -- the nodes, the pipe, the kernel control, and
 * the service it looks for last -- and, should a step fail, what the steps before it made taken away again: the
 * controller's driver powers the controller down then, and bluetoothd is not to find a service with nothing behind it.
 * (A second start, or one after a failed start, is refused or tried afresh.)
 */
int
board_bt_start(const struct board_bt_controller *given)
{
	thread_t thread;
	int error;

	if (controller != NULL) {
		return EBUSY;
	}
	PE_parse_boot_argn("bt_quirks", &quirks, sizeof(quirks));
	if ((error = nodes_make()) != 0) {
		return error;
	}
	if ((error = pipe_make()) != 0) {
		nodes_remove();
		return error;
	}
	if ((error = control_register()) != 0) {
		pipe_unmake();
		nodes_remove();
		return error;
	}
	if ((error = board_bt_publish()) != 0) {
		(void)ctl_deregister(control);
		control = NULL;
		pipe_unmake();
		nodes_remove();
		return error;
	}
	controller = given;
	if (kernel_thread_start(carry, NULL, &thread) != KERN_SUCCESS) {
		controller = NULL;
		board_bt_unpublish();
		(void)ctl_deregister(control);
		control = NULL;
		pipe_unmake();
		nodes_remove();
		return ENOMEM;
	}
	thread_deallocate(thread);
	say("bt: %s is Darwin's: service \"bluetooth\", /dev/btwake, the Skywalk pipe and com.apple.uart.sk.bluetooth made "
	    "(bt_quirks=%#x)\n", controller->name, quirks);
	return 0;
}
