/*
 * en1: the modem's packets as a network interface Darwin will use.
 *
 * Darwin's own cellular interface (pdp_ip0) is made by its baseband stack, which is not here. What
 * is here is the kernel's Ethernet-looking interface (hakios_net.c) and configd, which treats an
 * Ethernet interface whose link comes up the way it treats a USB Ethernet adapter: it runs DHCP
 * on it, takes the address, the router and the DNS servers it is given, and routes through it.
 * So the modem's IP packets (hakios_ipa.c, QMAP mux 1) are shown as Ethernet frames between two
 * made-up stations, the interface's and "the router's", and this file answers, in the kernel,
 * what the stack asks of the router: ARP for any address (everything is behind the router) and
 * DHCP with the data session's settings (the address and DNS servers the modem got from the
 * network, hakios_mpss.c, QMI WDS). Packets to the router go to the modem; packets from the modem
 * come from the router. IPv6 is not carried (the session is IPv4).
 *
 * The mask and the router given to DHCP are not the modem's: a cellular session is a point-to-point
 * link, and its "gateway" need not be in any subnet with the address, which DHCP cannot express.
 * The interface gets a /24 around its address with the router at its .1 (or .2): any router will
 * do, since every packet goes to the modem anyway. An address that would be a /24's own or its
 * broadcast address (x.y.z.0, x.y.z.255) gets as much shorter a prefix as it takes (cell_mask()).
 */
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/kpi_mbuf.h>
#include <sys/systm.h>
#include <sys/board/net.h>
#include <sys/board/ipa.h>

#define CELL_UNIT               1
#define CELL_MUX                1               /* the QMAP mux id the session is bound to (WDS Bind Mux Data Port) */

static const uint8_t cell_mac[6] = { 0x02, 0x68, 0x61, 0x6b, 0x69, 0x01 };      /* locally administered: "haki", 1 */
static const uint8_t router_mac[6] = { 0x02, 0x68, 0x61, 0x6b, 0x69, 0xfe };

static LCK_GRP_DECLARE(cell_lock_group, "hakios-cell");
static LCK_MTX_DECLARE(cell_lock, &cell_lock_group);
static struct {
	bool            up;
	uint32_t        address, mask, router, dns1, dns2, mtu;        /* host order; what DHCP gives out */
	uint32_t        modem_mask, modem_gateway;                      /* what the modem said, for the log */
} session;
static struct {
	uint64_t        arp, dhcp, to_modem, from_modem, dropped_out, dropped_in, not_ip4;
} cell_count;
static bool cell_attached, cell_trace;

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
 * The mask en1 is told: a /24 around the address (see the top of the file), or as much shorter as it takes for the
 * address not to be its network's own address or its broadcast address (a carrier's x.y.z.0 or x.y.z.255); /8 at most.
 */
static uint32_t
cell_mask(uint32_t address)
{
	for (unsigned bits = 24; bits >= 8; bits--) {
		uint32_t mask = 0xffffffffu << (32 - bits), host = address & ~mask;

		if (host != 0 && host != ~mask) {
			return mask;
		}
	}
	return 0xffffff00u;
}

/* ---- ARP: the router answers for everything ---------------------------------------------------- */

static bool
arp_answer(const uint8_t *frame, size_t length)
{
	const uint8_t *arp = frame + 14;
	uint8_t reply[42];

	/*
	 * Ethernet/IPv4, a request, not for our own address; and asked from the address the session has, or none (a probe):
	 * a request from an address of an earlier session is not answered, so that a lease of it is not taken as still good
	 */
	if (length < 42 || get32(arp) != 0x00010800 || arp[4] != 6 || arp[5] != 4 || arp[7] != 1 || get32(arp + 24) == session.address ||
	    (get32(arp + 14) != session.address && get32(arp + 14) != 0)) {
		return false;
	}
	memcpy(reply, frame + 6, 6);                    /* to the asker */
	memcpy(reply + 6, router_mac, 6);
	put16(reply + 12, 0x0806);
	put32(reply + 14, 0x00010800);
	reply[18] = 6; reply[19] = 4;
	put16(reply + 20, 2);                           /* a reply */
	memcpy(reply + 22, router_mac, 6);              /* "that address is me" */
	memcpy(reply + 28, arp + 24, 4);
	memcpy(reply + 32, arp + 8, 10);                /* the asker's addresses */
	cell_count.arp++;
	if (cell_trace) {
		printf("cell: ARP: %u.%u.%u.%u is the router\n", arp[24], arp[25], arp[26], arp[27]);
	}
	hakios_net_input_unit(CELL_UNIT, reply, sizeof(reply));
	return true;
}

/* ---- DHCP: the session's settings ---------------------------------------------------------------- */

#define DHCP_DISCOVER           1
#define DHCP_OFFER              2
#define DHCP_REQUEST            3
#define DHCP_ACK                5
#define DHCP_NAK                6

/*
 * Is this a DHCP request (UDP 68 -> 67, to everyone or to the router)? `udp` is set to its UDP header. A packet that is
 * not a whole datagram is not one, whatever its bytes where a port would be; nor is UDP to port 67 of a host beyond the
 * router, which is the modem's to carry like any other packet.
 */
static bool
dhcp_request(const uint8_t *frame, size_t length, const uint8_t **udp)
{
	const uint8_t *ip = frame + 14;
	uint32_t to;
	size_t ihl;

	/* (the flags' "more fragments" and the fragment's offset: both zero) */
	if (length < 14 + 20 + 8 + 240 || (ip[0] >> 4) != 4 || ip[9] != 17 || (ip[6] & 0x3f) != 0 || ip[7] != 0) {
		return false;
	}
	ihl = 4 * (ip[0] & 0xf);
	to = get32(ip + 16);
	if (ihl < 20 || 14 + ihl + 8 + 240 > length || (to != 0xffffffffu && to != session.router)) {
		return false;
	}
	*udp = ip + ihl;
	return ((*udp)[0] << 8 | (*udp)[1]) == 68 && ((*udp)[2] << 8 | (*udp)[3]) == 67;
}

/* The answer to a DHCP request: an offer for a discover, an acknowledgement for a request, with the session's settings. */
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
	/* the options: the message type, the address asked for */
	for (option = request + 240; option + 2 <= end && *option != 255; option += option[0] == 0 ? 1 : 2 + option[1]) {
		if (option[0] == 0) {
			continue;
		}
		if (option + 2 + option[1] > end) {
			break;
		}
		if (option[0] == 53 && option[1] == 1) {
			type = option[2];
		} else if (option[0] == 50 && option[1] == 4) {
			wanted = get32(option + 2);
		}
	}
	if (type != DHCP_DISCOVER && type != DHCP_REQUEST) {
		return false;
	}
	kind = type == DHCP_DISCOVER ? DHCP_OFFER : wanted != 0 && wanted != session.address ? DHCP_NAK : DHCP_ACK;
	bzero(reply, sizeof(reply));
	/* BOOTP */
	bootp[0] = 2; bootp[1] = 1; bootp[2] = 6;      /* a reply, Ethernet */
	memcpy(bootp + 4, request + 4, 4);              /* the transaction */
	memcpy(bootp + 10, request + 10, 2);            /* the flags (broadcast) */
	if (kind != DHCP_NAK) {
		put32(bootp + 16, session.address);     /* yiaddr */
		put32(bootp + 20, session.router);      /* siaddr */
	}
	memcpy(bootp + 28, request + 28, 16);           /* the client's hardware address */
	put32(bootp + 236, 0x63825363);
	o = bootp + 240;
	*o++ = 53; *o++ = 1; *o++ = (uint8_t)kind;
	*o++ = 54; *o++ = 4; put32(o, session.router); o += 4;      /* the server: the router */
	if (kind != DHCP_NAK) {
		*o++ = 51; *o++ = 4; put32(o, 86400); o += 4;           /* a day's lease */
		*o++ = 1; *o++ = 4; put32(o, session.mask); o += 4;
		*o++ = 3; *o++ = 4; put32(o, session.router); o += 4;
		if (session.dns1 != 0 || session.dns2 != 0) {
			*o++ = 6; *o++ = (uint8_t)(4 * ((session.dns1 != 0) + (session.dns2 != 0)));
			if (session.dns1 != 0) { put32(o, session.dns1); o += 4; }
			if (session.dns2 != 0) { put32(o, session.dns2); o += 4; }
		}
		if (session.mtu >= 576 && session.mtu <= 1500) {
			*o++ = 26; *o++ = 2; put16(o, session.mtu); o += 2;
		}
	}
	*o++ = 255;
	size = (size_t)(o - bootp);
	if (size < 300) {
		size = 300;                             /* the traditional minimum, padded with zeros */
	}
	/* UDP 67 -> 68, no checksum */
	put16(rudp, 67); put16(rudp + 2, 68); put16(rudp + 4, (uint32_t)(8 + size)); put16(rudp + 6, 0);
	/* IPv4 from the router to everyone */
	ip[0] = 0x45; ip[1] = 0;
	put16(ip + 2, (uint32_t)(20 + 8 + size));
	put16(ip + 4, 0); put16(ip + 6, 0);
	ip[8] = 64; ip[9] = 17;
	put32(ip + 12, session.router);
	put32(ip + 16, 0xffffffff);
	put16(ip + 10, checksum(ip, 20));
	/* Ethernet to the client */
	memcpy(reply, request + 28, 6);
	memcpy(reply + 6, router_mac, 6);
	put16(reply + 12, 0x0800);
	cell_count.dhcp++;
	printf("cell: DHCP %s answered with %s: %u.%u.%u.%u/%u.%u.%u.%u, router %u.%u.%u.%u, DNS %u.%u.%u.%u %u.%u.%u.%u, MTU %u\n",
	    type == DHCP_DISCOVER ? "discover" : "request", kind == DHCP_OFFER ? "an offer" : kind == DHCP_ACK ? "an acknowledgement" : "a refusal",
	    session.address >> 24, (session.address >> 16) & 0xff, (session.address >> 8) & 0xff, session.address & 0xff,
	    session.mask >> 24, (session.mask >> 16) & 0xff, (session.mask >> 8) & 0xff, session.mask & 0xff,
	    session.router >> 24, (session.router >> 16) & 0xff, (session.router >> 8) & 0xff, session.router & 0xff,
	    session.dns1 >> 24, (session.dns1 >> 16) & 0xff, (session.dns1 >> 8) & 0xff, session.dns1 & 0xff,
	    session.dns2 >> 24, (session.dns2 >> 16) & 0xff, (session.dns2 >> 8) & 0xff, session.dns2 & 0xff, session.mtu);
	hakios_net_input_unit(CELL_UNIT, reply, 14 + 20 + 8 + size);
	return true;
}

/* ---- the link ----------------------------------------------------------------------------------- */

/*
 * Is this IPv4 frame one for the modem? Only what the stack sends through the router: not what it sends to everyone
 * on the link -- the limited broadcast, the subnet's broadcast, a multicast group (mDNS, SSDP, NetBIOS) -- which a LAN
 * keeps to itself, and which the modem would carry to the network, if only to be dropped there.
 */
static bool
cell_routed(const uint8_t *frame, size_t length)
{
	uint32_t to;

	if (length < 14 + 20 || memcmp(frame, router_mac, 6) != 0) {
		return false;
	}
	to = get32(frame + 14 + 16);
	return (to >> 28) != 0xe && to != 0xffffffffu && to != (session.address | ~session.mask);
}

/* A frame from the stack: answered here, or its IP packet to the modem. */
static bool
cell_transmit(mbuf_t packet, size_t length)
{
	uint8_t frame[HAKIOS_NET_FRAME_MAX];
	const uint8_t *udp;
	uint32_t type;
	bool ok;

	if (length < 14 || length > sizeof(frame) || mbuf_copydata(packet, 0, length, frame) != 0) {
		return false;
	}
	type = (uint32_t)frame[12] << 8 | frame[13];
	lck_mtx_lock(&cell_lock);
	if (type == 0x0806) {
		ok = arp_answer(frame, length);
	} else if (type == 0x0800 && dhcp_request(frame, length, &udp)) {
		ok = session.up && dhcp_answer(frame, length, udp);
		if (!ok) {
			cell_count.dropped_out++;       /* (a release, say, or no session yet: not the modem's either) */
		}
	} else if (type == 0x0800) {
		ok = session.up && cell_routed(frame, length) && hakios_ipa_transmit(CELL_MUX, frame + 14, length - 14);
		if (ok) {
			cell_count.to_modem++;
		} else {
			cell_count.dropped_out++;
		}
	} else {
		cell_count.not_ip4++;                   /* IPv6 and the rest: not carried */
		ok = false;
	}
	lck_mtx_unlock(&cell_lock);
	return ok;
}

/* A packet from the modem: an Ethernet frame from the router made around it. */
static void
cell_receive(uint8_t mux, const uint8_t *packet, size_t length)
{
	uint8_t frame[HAKIOS_NET_FRAME_MAX];
	uint32_t version = length > 0 ? packet[0] >> 4 : 0;

	/* (another mux is another session's, one this interface does not stand for) */
	if (mux != CELL_MUX || length < 20 || length + 14 > sizeof(frame) || (version != 4 && version != 6)) {
		cell_count.dropped_in++;
		return;
	}
	if (cell_trace) {
		printf("cell: %zu bytes from the modem on mux %u (IPv%u)\n", length, mux, version);
	}
	memcpy(frame, cell_mac, 6);
	memcpy(frame + 6, router_mac, 6);
	put16(frame + 12, version == 4 ? 0x0800 : 0x86dd);
	memcpy(frame + 14, packet, length);
	cell_count.from_modem++;
	hakios_net_input_unit(CELL_UNIT, frame, 14 + length);
}

static unsigned
cell_poll(void)
{
	static unsigned ticks;
	unsigned got = hakios_ipa_poll(cell_receive);
	bool stopped = false;

	/*
	 * The IPA's data path stopped under a session (the modem driver stops it at the run's end, and a kept modem's session
	 * is not said to be over): nothing more can pass, so the link goes down as if the session had ended, and the stack
	 * stops sending on it (nor are its ARP and DHCP answered). A session told afterwards brings it up again.
	 */
	if (session.up && !hakios_ipa_data_ready()) {
		lck_mtx_lock(&cell_lock);
		stopped = session.up && !hakios_ipa_data_ready();
		if (stopped) {
			session.up = false;
		}
		lck_mtx_unlock(&cell_lock);
	}
	if (stopped) {
		printf("cell: the IPA's data path is stopped: en1's link is taken down\n");
		hakios_net_link_state_unit(CELL_UNIT, false);
	}

	if (++ticks % 30000 == 0 && (cell_count.to_modem != 0 || cell_count.from_modem != 0 || cell_count.dropped_out != 0)) {
		char line[256];

		hakios_ipa_status(line, sizeof(line));
		printf("cell: so far %llu packets to the modem, %llu from it; dropped %llu out, %llu in; %llu not IPv4; ARP %llu, DHCP %llu; "
		    "IPA: %s\n", cell_count.to_modem, cell_count.from_modem, cell_count.dropped_out, cell_count.dropped_in, cell_count.not_ip4,
		    cell_count.arp, cell_count.dhcp, line);
	}
	return got;
}

static struct hakios_net_link cell_link = {
	.name = "the modem through the IPA",
	.unit = CELL_UNIT,
	.address = { 0x02, 0x68, 0x61, 0x6b, 0x69, 0x01 },
	.wireless = false,
	.up = false,
	.transmit = cell_transmit,
	.poll = cell_poll,
};

void
hakios_cell_session(bool up, uint32_t address, uint32_t mask, uint32_t gateway, uint32_t dns1, uint32_t dns2, uint32_t mtu)
{
	bool was_up, changed;
	uint32_t router;

	lck_mtx_lock(&cell_lock);
	was_up = session.up;
	changed = session.address != address || session.dns1 != dns1 || session.dns2 != dns2 || session.mtu != mtu;
	session.modem_mask = mask;
	session.modem_gateway = gateway;
	mask = cell_mask(address);
	router = (address & mask) | 1;
	if (router == address) {
		router = (address & mask) | 2;
	}
	session.address = address;
	session.mask = mask;
	session.router = router;
	session.dns1 = dns1;
	session.dns2 = dns2;
	session.mtu = mtu;
	session.up = up && address != 0;
	lck_mtx_unlock(&cell_lock);
	printf("cell: the data session is %s: %u.%u.%u.%u (the modem's mask %u.%u.%u.%u and gateway %u.%u.%u.%u; en1 is told /%u and router "
	    "%u.%u.%u.%u), DNS %u.%u.%u.%u %u.%u.%u.%u, MTU %u%s\n", session.up ? "UP" : "down",
	    address >> 24, (address >> 16) & 0xff, (address >> 8) & 0xff, address & 0xff,
	    session.modem_mask >> 24, (session.modem_mask >> 16) & 0xff, (session.modem_mask >> 8) & 0xff, session.modem_mask & 0xff,
	    gateway >> 24, (gateway >> 16) & 0xff, (gateway >> 8) & 0xff, gateway & 0xff, (unsigned)__builtin_popcount(mask),
	    router >> 24, (router >> 16) & 0xff, (router >> 8) & 0xff, router & 0xff,
	    dns1 >> 24, (dns1 >> 16) & 0xff, (dns1 >> 8) & 0xff, dns1 & 0xff, dns2 >> 24, (dns2 >> 16) & 0xff, (dns2 >> 8) & 0xff, dns2 & 0xff,
	    mtu, cell_attached ? "" : " (no en1: the IPA's data path is not up)");
	if (cell_attached) {
		if (was_up && session.up && changed) {
			/*
			 * (another address, other DNS servers or another MTU while the link stays up: down and up again, so that DHCP
			 * is asked anew, not the old lease kept until it is renewed)
			 */
			hakios_net_link_state_unit(CELL_UNIT, false);
		}
		hakios_net_link_state_unit(CELL_UNIT, session.up);
	}
}

void
hakios_cell_attach(void)
{
	uint32_t t = 0;

	if (cell_attached || !hakios_ipa_data_ready()) {
		return;
	}
	PE_parse_boot_argn("modem_ipa_data_trace", &t, sizeof(t));
	cell_trace = t != 0;
	cell_attached = hakios_net_attach(&cell_link);
	if (cell_attached && session.up) {
		hakios_net_link_state_unit(CELL_UNIT, true);
	}
}
