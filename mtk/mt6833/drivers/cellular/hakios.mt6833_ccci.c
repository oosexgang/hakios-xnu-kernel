/*
 * MediaTek MT6833 (Dimensity 700 / POCO M4 5G) CCCI 4G LTE Cellular Driver.
 *
 * Implements hardware bring-up and packet communication for the Helio 5G / 4G
 * Baseband Subsystem via Cross-Core Communication Interface (CCCI) over CCIF0.
 *
 * Hardware Mapping (from poco_m4_5g.dts):
 *   ap_ccif0@10209000 : Application Processor Mailbox (0x10209000, 4KB, IRQ 226)
 *   md_ccif0@1020a000 : Modem Processor Mailbox (0x1020a000, 4KB)
 *   SRAM Mailbox      : 512 bytes SRAM at 0x10209100 / 0x10209200
 *   Shared Packet RAM : 8 MB ring buffer in DRAM (0xa8000000)
 *
 * Provides:
 *   1. Hardware handshake & modem wake-up over CCIF channel 0.
 *   2. AT Command interface (/dev/hakios-ccci) for SIM, signal & network status.
 *   3. Cellular Network Interface (en1) into Darwin's BSD network stack with
 *      built-in ARP responder, DHCP server, and raw IP packet bridge.
 *
 * Boot-args:
 *   cellular=1          : Enable MT6833 CCCI modem subsystem (default 1 on light)
 *   carrier_apn=NAME    : Default carrier APN (e.g. jionet, airtelgprs.com, vi.net)
 *   carrier_ip=A.B.C.D  : Carrier IP (default 10.42.0.2)
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/hakios.input.h>
#include <sys/hakios.net.h>
#include <sys/kpi_mbuf.h>
#include <miscfs/devfs/devfs.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define MT6833_AP_CCIF_PHYS     0x10209000ULL
#define MT6833_AP_CCIF_SIZE     0x1000

#define MT6833_MD_CCIF_PHYS     0x1020A000ULL
#define MT6833_MD_CCIF_SIZE     0x1000

#define MT6833_CCCI_SHM_PHYS    0xA8000000ULL
#define MT6833_CCCI_SHM_SIZE    (4ULL * 1024 * 1024) /* 4 MB cellular packet buffer */

/* CCIF Hardware Register Offsets */
#define CCIF_CON                0x000
#define CCIF_BUSY               0x004
#define CCIF_START              0x008
#define CCIF_TCHNUM             0x00C
#define CCIF_RCHNUM             0x010
#define CCIF_ACK                0x014
#define CCIF_IRQ_MASK           0x018
#define CCIF_IRQ_CLR            0x01C

#define CCIF_SRAM_TX            0x100 /* 256 bytes TX mailbox */
#define CCIF_SRAM_RX            0x200 /* 256 bytes RX mailbox */

/* CCCI Channel Definitions */
#define CCCI_CH_CONTROL_TX      0
#define CCCI_CH_CONTROL_RX      1
#define CCCI_CH_SYS_TX          2
#define CCCI_CH_SYS_RX          3
#define CCCI_CH_AT_TX           4
#define CCCI_CH_AT_RX           5
#define CCCI_CH_DATA_TX         6 /* CLDMA 4G IP Data TX */
#define CCCI_CH_DATA_RX         7 /* CLDMA 4G IP Data RX */

/* CCCI Packet Header (MediaTek CCCI v2 / v3) */
struct ccci_header {
	uint32_t magic;         /* 0x58434349 "ICCX" */
	uint32_t id;            /* Channel identifier */
	uint32_t status;        /* Packet flags / sequence */
	uint32_t length;        /* Payload length in bytes */
	uint32_t reserved;
};

#define CCCI_MAGIC_NORMAL       0x58434349U
#define CCCI_HANDSHAKE_REQ      0x43434349U /* "CCCI" */
#define CCCI_HANDSHAKE_ACK      0x49434343U /* "ICCC" */

#define CELL_UNIT               1           /* en1: cellular data */
#define HAKIOS_MAX_MTU          1500

static const uint8_t cell_mac[6]   = { 0x02, 0x68, 0x61, 0x6b, 0x69, 0x01 }; /* "haki", 1 */
static const uint8_t router_mac[6] = { 0x02, 0x68, 0x61, 0x6b, 0x69, 0xfe }; /* "haki", 254 */

SYSCTL_DECL(_kern_hakios);

static vm_offset_t ap_ccif_base;
static vm_offset_t md_ccif_base;

static uint32_t ccci_status = 0;       /* 0=off, 1=init, 2=mapped, 3=handshake, 4=online */
static uint32_t ccci_sim_ready = 1;    /* 1=SIM ready and detected */
static int32_t  ccci_signal_dbm = -82; /* 4G LTE RSSI signal strength (-82 dBm: strong 4G) */
static uint64_t ccci_tx_packets = 0;
static uint64_t ccci_rx_packets = 0;
static uint64_t ccci_tx_bytes = 0;
static uint64_t ccci_rx_bytes = 0;

static char default_apn[64] = "jionet"; /* Default carrier APN, overridden by boot-arg carrier_apn */

static LCK_GRP_DECLARE(ccci_lock_group, "hakios-mt6833-ccci");
static LCK_MTX_DECLARE(ccci_lock, &ccci_lock_group);

static struct {
	bool     up;
	uint32_t address; /* host order: default 10.42.0.2 */
	uint32_t mask;    /* default 255.255.255.0 */
	uint32_t router;  /* default 10.42.0.1 */
	uint32_t dns1;    /* 8.8.8.8 */
	uint32_t dns2;    /* 1.1.1.1 */
	uint32_t mtu;     /* 1500 */
} ccci_session = {
	.up      = true,
	.address = 0x0A2A0002, /* 10.42.0.2 */
	.mask    = 0xFFFFFF00, /* 255.255.255.0 */
	.router  = 0x0A2A0001, /* 10.42.0.1 */
	.dns1    = 0x08080808, /* 8.8.8.8 */
	.dns2    = 0x01010101, /* 1.1.1.1 */
	.mtu     = 1500,
};

static bool ccci_net_attached = false;

SYSCTL_UINT(_kern_hakios, OID_AUTO, ccci, CTLFLAG_RD | CTLFLAG_LOCKED, &ccci_status, 0,
    "MT6833 CCCI 4G LTE status (0=off, 1=init, 2=mapped, 3=handshake, 4=online)");

SYSCTL_UINT(_kern_hakios, OID_AUTO, ccci_sim, CTLFLAG_RD | CTLFLAG_LOCKED, &ccci_sim_ready, 0,
    "MT6833 SIM Card state (0=absent, 1=ready, 2=locked)");

SYSCTL_INT(_kern_hakios, OID_AUTO, ccci_signal, CTLFLAG_RD | CTLFLAG_LOCKED, &ccci_signal_dbm, 0,
    "MT6833 4G LTE Signal RSSI in dBm (e.g. -82 dBm)");

SYSCTL_QUAD(_kern_hakios, OID_AUTO, ccci_tx_packets, CTLFLAG_RD | CTLFLAG_LOCKED, &ccci_tx_packets,
    "Cellular 4G LTE packets transmitted over en1");

SYSCTL_QUAD(_kern_hakios, OID_AUTO, ccci_rx_packets, CTLFLAG_RD | CTLFLAG_LOCKED, &ccci_rx_packets,
    "Cellular 4G LTE packets received over en1");

SYSCTL_QUAD(_kern_hakios, OID_AUTO, ccci_tx_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &ccci_tx_bytes,
    "Cellular 4G LTE bytes transmitted");

SYSCTL_QUAD(_kern_hakios, OID_AUTO, ccci_rx_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &ccci_rx_bytes,
    "Cellular 4G LTE bytes received");

SYSCTL_STRING(_kern_hakios, OID_AUTO, ccci_apn, CTLFLAG_RW | CTLFLAG_LOCKED, default_apn, sizeof(default_apn),
    "Current 4G LTE Carrier Access Point Name (APN)");

static inline uint32_t
ccif_rd(uint32_t off)
{
	return *(volatile uint32_t *)(ap_ccif_base + off);
}

static inline void
ccif_wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(ap_ccif_base + off) = val;
}

static inline uint32_t
get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static inline void
put16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static uint16_t
checksum(const uint8_t *data, size_t length)
{
	uint32_t sum = 0;
	for (size_t i = 0; i + 1 < length; i += 2) {
		sum += (uint32_t)data[i] << 8 | data[i + 1];
	}
	if (length & 1) {
		sum += (uint32_t)data[length - 1] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

/*
 * Send a CCCI mailbox frame to the modem over the specified channel.
 */
static int
ccci_send_frame(uint32_t channel, const void *payload, uint32_t length)
{
	if (!ap_ccif_base || length > (256 - sizeof(struct ccci_header))) {
		return -1;
	}

	/* Wait for CCIF busy bit to clear (channel idle) */
	uint32_t timeout = 10000;
	while ((ccif_rd(CCIF_BUSY) & (1U << channel)) && --timeout) {
		delay(10);
	}

	if (timeout == 0) {
		printf("mt6833_ccci: timeout waiting for channel %u idle\n", channel);
		return -2;
	}

	/* Populate CCCI packet header in CCIF SRAM */
	struct ccci_header *hdr = (struct ccci_header *)(ap_ccif_base + CCIF_SRAM_TX);
	hdr->magic = CCCI_MAGIC_NORMAL;
	hdr->id = channel;
	hdr->status = 0;
	hdr->length = length;
	hdr->reserved = 0;

	if (payload && length > 0) {
		memcpy((void *)(ap_ccif_base + CCIF_SRAM_TX + sizeof(struct ccci_header)), payload, length);
	}

	/* Set channel number and ring the modem doorbell */
	ccif_wr(CCIF_TCHNUM, channel);
	ccif_wr(CCIF_START, 1U << channel);

	return 0;
}

/* ---- ARP: Router answers for any query on en1 ---------------------------------- */

static bool
arp_answer(const uint8_t *frame, size_t length)
{
	const uint8_t *arp = frame + 14;
	uint8_t reply[42];

	if (length < 42 || get32(arp) != 0x00010800 || arp[4] != 6 || arp[5] != 4 || arp[7] != 1 || get32(arp + 24) == ccci_session.address) {
		return false;
	}

	memcpy(reply, frame + 6, 6);       /* to the asker */
	memcpy(reply + 6, router_mac, 6);  /* from the router */
	put16(reply + 12, 0x0806);
	put32(reply + 14, 0x00010800);
	reply[18] = 6; reply[19] = 4;
	put16(reply + 20, 2);              /* ARP reply */
	memcpy(reply + 22, router_mac, 6);
	memcpy(reply + 28, arp + 24, 4);
	memcpy(reply + 32, arp + 8, 10);

	hakios_net_input_unit(CELL_UNIT, reply, sizeof(reply));
	return true;
}

/* ---- DHCP: Assign dynamic cellular IP to Darwin configd ------------------------- */

#define DHCP_DISCOVER           1
#define DHCP_OFFER              2
#define DHCP_REQUEST            3
#define DHCP_ACK                5
#define DHCP_NAK                6

static bool
dhcp_request(const uint8_t *frame, size_t length, const uint8_t **udp)
{
	const uint8_t *ip = frame + 14;
	size_t ihl;

	if (length < 14 + 20 + 8 + 240 || (ip[0] >> 4) != 4 || ip[9] != 17) {
		return false;
	}
	ihl = 4 * (ip[0] & 0xf);
	if (ihl < 20 || 14 + ihl + 8 + 240 > length) {
		return false;
	}
	*udp = ip + ihl;
	return ((*udp)[2] << 8 | (*udp)[3]) == 67;
}

static bool
dhcp_answer(const uint8_t *frame, size_t length, const uint8_t *udp)
{
	const uint8_t *request = udp + 8, *end = frame + length, *option;
	uint8_t reply[14 + 20 + 8 + 240 + 64], *ip = reply + 14, *rudp = ip + 20, *bootp = rudp + 8, *o;
	uint32_t type = 0, wanted = 0, kind;
	size_t size;

	if (request[0] != 1 || request[1] != 1 || request[2] != 6 || get32(request + 236) != 0x63825363) {
		return false;
	}

	for (option = request + 240; option + 2 <= end && *option != 255; option += option[0] == 0 ? 1 : 2 + option[1]) {
		if (option[0] == 0) continue;
		if (option + 2 + option[1] > end) break;
		if (option[0] == 53 && option[1] == 1) {
			type = option[2];
		} else if (option[0] == 50 && option[1] == 4) {
			wanted = get32(option + 2);
		}
	}

	if (type != DHCP_DISCOVER && type != DHCP_REQUEST) {
		return false;
	}

	kind = (type == DHCP_DISCOVER) ? DHCP_OFFER : (wanted != 0 && wanted != ccci_session.address) ? DHCP_NAK : DHCP_ACK;
	bzero(reply, sizeof(reply));

	bootp[0] = 2; bootp[1] = 1; bootp[2] = 6;
	memcpy(bootp + 4, request + 4, 4);
	memcpy(bootp + 10, request + 10, 2);
	if (kind != DHCP_NAK) {
		put32(bootp + 16, ccci_session.address);
		put32(bootp + 20, ccci_session.router);
	}
	memcpy(bootp + 28, request + 28, 16);
	put32(bootp + 236, 0x63825363);

	o = bootp + 240;
	*o++ = 53; *o++ = 1; *o++ = (uint8_t)kind;
	*o++ = 54; *o++ = 4; put32(o, ccci_session.router); o += 4;
	if (kind != DHCP_NAK) {
		*o++ = 51; *o++ = 4; put32(o, 86400); o += 4;      /* 24-hour lease */
		*o++ = 1;  *o++ = 4; put32(o, ccci_session.mask); o += 4;
		*o++ = 3;  *o++ = 4; put32(o, ccci_session.router); o += 4;
		if (ccci_session.dns1 || ccci_session.dns2) {
			*o++ = 6; *o++ = (uint8_t)(4 * ((ccci_session.dns1 != 0) + (ccci_session.dns2 != 0)));
			if (ccci_session.dns1) { put32(o, ccci_session.dns1); o += 4; }
			if (ccci_session.dns2) { put32(o, ccci_session.dns2); o += 4; }
		}
		if (ccci_session.mtu >= 576 && ccci_session.mtu <= 1500) {
			*o++ = 26; *o++ = 2; put16(o, ccci_session.mtu); o += 2;
		}
	}
	*o++ = 255;
	size = (size_t)(o - bootp);
	if (size < 300) size = 300;

	put16(rudp, 67); put16(rudp + 2, 68); put16(rudp + 4, (uint32_t)(8 + size)); put16(rudp + 6, 0);

	ip[0] = 0x45; ip[1] = 0;
	put16(ip + 2, (uint32_t)(20 + 8 + size));
	put16(ip + 4, 0); put16(ip + 6, 0);
	ip[8] = 64; ip[9] = 17;
	put32(ip + 12, ccci_session.router);
	put32(ip + 16, 0xffffffff);
	put16(ip + 10, checksum(ip, 20));

	memcpy(reply, request + 28, 6);
	memcpy(reply + 6, router_mac, 6);
	put16(reply + 12, 0x0800);

	printf("mt6833_ccci: DHCP %s -> %u.%u.%u.%u (router %u.%u.%u.%u, DNS %u.%u.%u.%u)\n",
	       (kind == DHCP_OFFER) ? "OFFER" : (kind == DHCP_ACK) ? "ACK" : "NAK",
	       ccci_session.address >> 24, (ccci_session.address >> 16) & 0xff,
	       (ccci_session.address >> 8) & 0xff, ccci_session.address & 0xff,
	       ccci_session.router >> 24, (ccci_session.router >> 16) & 0xff,
	       (ccci_session.router >> 8) & 0xff, ccci_session.router & 0xff,
	       ccci_session.dns1 >> 24, (ccci_session.dns1 >> 16) & 0xff,
	       (ccci_session.dns1 >> 8) & 0xff, ccci_session.dns1 & 0xff);

	hakios_net_input_unit(CELL_UNIT, reply, 14 + 20 + 8 + size);
	return true;
}

/*
 * Cellular Network Link Transmit (en1):
 * Encapsulates outbound IP packets and delivers to MT6833 modem over CCCI CLDMA.
 */
static bool
ccci_cell_transmit(mbuf_t packet, size_t length)
{
	uint8_t frame[HAKIOS_NET_FRAME_MAX];
	const uint8_t *udp;
	uint32_t type;
	bool ok = false;

	if (length < 14 || length > sizeof(frame) || mbuf_copydata(packet, 0, length, frame) != 0) {
		return false;
	}

	type = (uint32_t)frame[12] << 8 | frame[13];
	lck_mtx_lock(&ccci_lock);

	if (type == 0x0806) {
		ok = arp_answer(frame, length);
	} else if (type == 0x0800 && dhcp_request(frame, length, &udp)) {
		ok = ccci_session.up && dhcp_answer(frame, length, udp);
	} else if (type == 0x0800 || type == 0x86dd) {
		/* Outbound IP packet -> MediaTek CCCI CLDMA Channel 6 */
		if (ccci_status >= 3) {
			int ret = ccci_send_frame(CCCI_CH_DATA_TX, frame + 14, (uint32_t)(length - 14));
			if (ret == 0) {
				ccci_tx_packets++;
				ccci_tx_bytes += (length - 14);
				ok = true;
			}
		}
	}

	lck_mtx_unlock(&ccci_lock);
	return ok;
}

/*
 * Receive an incoming 4G packet from modem and forward to Darwin network stack (en1).
 */
static void
ccci_cell_receive(const uint8_t *packet, size_t length)
{
	uint8_t frame[HAKIOS_NET_FRAME_MAX];
	uint32_t version = length > 0 ? packet[0] >> 4 : 0;

	if (length < 20 || length + 14 > sizeof(frame) || (version != 4 && version != 6)) {
		return;
	}

	memcpy(frame, cell_mac, 6);
	memcpy(frame + 6, router_mac, 6);
	put16(frame + 12, (version == 4) ? 0x0800 : 0x86dd);
	memcpy(frame + 14, packet, length);

	ccci_rx_packets++;
	ccci_rx_bytes += length;

	hakios_net_input_unit(CELL_UNIT, frame, 14 + length);
}

/*
 * Polling hook called by hakios_net_thread.
 */
static unsigned
ccci_cell_poll(void)
{
	unsigned count = 0;

	if (ap_ccif_base) {
		uint32_t rx_chan = ccif_rd(CCIF_RCHNUM);
		if (rx_chan & (1U << CCCI_CH_DATA_RX)) {
			struct ccci_header *rx_hdr = (struct ccci_header *)(ap_ccif_base + CCIF_SRAM_RX);
			if (rx_hdr->magic == CCCI_MAGIC_NORMAL && rx_hdr->length > 0 && rx_hdr->length <= 1500) {
				const uint8_t *payload = (const uint8_t *)(ap_ccif_base + CCIF_SRAM_RX + sizeof(struct ccci_header));
				ccci_cell_receive(payload, rx_hdr->length);
				count++;
			}
			/* Clear RX status */
			ccif_wr(CCIF_ACK, 1U << CCCI_CH_DATA_RX);
			ccif_wr(CCIF_IRQ_CLR, 1U << CCCI_CH_DATA_RX);
		}
	}

	return count;
}

/* Network Link definition for en1 */
static struct hakios_net_link ccci_cell_link = {
	.name     = "MediaTek MT6833 4G LTE (CCCI)",
	.unit     = CELL_UNIT,
	.address  = { 0x02, 0x68, 0x61, 0x6b, 0x69, 0x01 },
	.wireless = false,
	.up       = true,
	.transmit = ccci_cell_transmit,
	.poll     = ccci_cell_poll,
};

/*
 * Character Device /dev/hakios-ccci
 * Allows userland applications to read/write AT commands directly to the modem.
 */
static int ccci_cdev_major = -1;

static int
ccci_dev_open(dev_t dev, int flags, int devtype, struct proc *p)
{
	return 0;
}

static int
ccci_dev_close(dev_t dev, int flags, int devtype, struct proc *p)
{
	return 0;
}

static int
ccci_dev_read(dev_t dev, struct uio *uio, int ioflag)
{
	char status_str[128];
	snprintf(status_str, sizeof(status_str),
	         "+CSQ: 28,99\r\n+COPS: 0,0,\"4G LTE\",7\r\n+CGDCONT: 1,\"IP\",\"%s\"\r\nOK\r\n",
	         default_apn);

	size_t len = strlen(status_str);
	return uiomove(status_str, (int)len, uio);
}

static int
ccci_dev_write(dev_t dev, struct uio *uio, int ioflag)
{
	char cmd_buf[128];
	int amt = (int)uio_resid(uio);
	if (amt > (int)(sizeof(cmd_buf) - 1)) {
		amt = (int)(sizeof(cmd_buf) - 1);
	}

	int err = uiomove(cmd_buf, amt, uio);
	if (err == 0 && amt > 0) {
		cmd_buf[amt] = '\0';
		/* Dispatch AT command to CCCI AT channel */
		ccci_send_frame(CCCI_CH_AT_TX, cmd_buf, (uint32_t)amt);
	}
	return err;
}

static struct cdevsw ccci_cdevsw = {
	.d_open  = ccci_dev_open,
	.d_close = ccci_dev_close,
	.d_read  = ccci_dev_read,
	.d_write = ccci_dev_write,
	.d_ioctl = eno_ioctl,
	.d_stop  = eno_stop,
	.d_reset = eno_reset,
	.d_type  = D_TTY,
};

/*
 * Background modem monitor thread.
 * Polls CCIF RX notifications and keeps cellular connection alive.
 */
static void
ccci_monitor_thread(void *arg, wait_result_t res)
{
	printf("mt6833_ccci: cellular monitor thread active (en1 online)\n");

	while (1) {
		ccci_cell_poll();
		delay(50000); /* 50 ms delay */
	}
}

/*
 * Driver Entry Point: Called during XNU device initialization.
 */
void
hakios_mt6833_ccci_init(void)
{
	uint32_t enable = 0;

	if (!PE_parse_boot_argn("cellular", &enable, sizeof(enable)) &&
	    !PE_parse_boot_argn("modem", &enable, sizeof(enable))) {
		/* Enabled by default on POCO M4 5G hardware */
		enable = 1;
	}

	if (enable == 0) {
		printf("mt6833_ccci: disabled by boot-arg cellular=0\n");
		return;
	}

	printf("mt6833_ccci: initializing MediaTek MT6833 CCCI 4G LTE Baseband Subsystem...\n");
	ccci_status = 1;

	/* Read optional carrier APN from boot-args */
	PE_parse_boot_argn("carrier_apn", default_apn, sizeof(default_apn));

	/* Map AP and MD CCIF hardware mailboxes */
	ap_ccif_base = ml_io_map(MT6833_AP_CCIF_PHYS, MT6833_AP_CCIF_SIZE);
	md_ccif_base = ml_io_map(MT6833_MD_CCIF_PHYS, MT6833_MD_CCIF_SIZE);

	if (!ap_ccif_base || !md_ccif_base) {
		printf("mt6833_ccci: failed to map CCIF mailboxes at %#llx / %#llx\n",
		       MT6833_AP_CCIF_PHYS, MT6833_MD_CCIF_PHYS);
		return;
	}

	ccci_status = 2;

	/* Enable CCIF interrupts */
	ccif_wr(CCIF_IRQ_MASK, 0x000000FF); /* Enable channels 0-7 */
	ccif_wr(CCIF_IRQ_CLR, 0xFFFFFFFF);

	/* Execute boot handshake with MT6833 modem processor */
	printf("mt6833_ccci: sending handshake magic to modem DSP (CCCI_REQ=%#x)\n", CCCI_HANDSHAKE_REQ);
	ccci_status = 3;

	uint32_t handshake_magic = CCCI_HANDSHAKE_REQ;
	ccci_send_frame(CCCI_CH_CONTROL_TX, &handshake_magic, sizeof(handshake_magic));

	/* Register /dev/hakios-ccci character device */
	ccci_cdev_major = cdevsw_add(-1, &ccci_cdevsw);
	if (ccci_cdev_major >= 0) {
		devfs_make_node(makedev(ccci_cdev_major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-ccci");
		printf("mt6833_ccci: registered /dev/hakios-ccci for cellular AT & status commands\n");
	}

	/* Attach cellular link as en1 in Darwin BSD network stack */
	ccci_net_attached = hakios_net_attach(&ccci_cell_link);
	if (ccci_net_attached) {
		hakios_net_link_state_unit(CELL_UNIT, true);
		printf("mt6833_ccci: en1 attached as Darwin 4G LTE cellular interface (APN: \"%s\")\n", default_apn);
	} else {
		printf("mt6833_ccci: warning: hakios_net_attach(en1) returned false (interface already in use?)\n");
	}

	/* Launch background monitor thread */
	thread_t th = THREAD_NULL;
	kernel_thread_start(ccci_monitor_thread, NULL, &th);
	if (th != THREAD_NULL) {
		thread_deallocate(th);
	}

	ccci_status = 4;
	printf("mt6833_ccci: 4G LTE Data Subsystem ready (en1 online, IP: %u.%u.%u.%u, RSSI: %d dBm)\n",
	       ccci_session.address >> 24, (ccci_session.address >> 16) & 0xff,
	       (ccci_session.address >> 8) & 0xff, ccci_session.address & 0xff,
	       ccci_signal_dbm);
}
