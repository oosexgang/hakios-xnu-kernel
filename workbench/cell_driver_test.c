/*
 * The modem's interface (drivers/board/cell.c), run as a host program: the frames the stack sends on en1 and the
 * packets the modem sends back, with the IPA and the interface replaced by stand-ins that note every packet sent to the
 * modem, every frame handed to the stack and every change of the link. What is tested: only what is sent to the router
 * goes to the modem, not the link's broadcasts and multicasts; a DHCP request is one from the client's port to the
 * router or to everyone, in a packet that is not a fragment, and what only looks like one is sent on; a request not
 * answered is counted; the link goes down once the IPA's data path is stopped; a packet on another mux is not taken;
 * a new session with other DNS servers or another MTU bounces the link, so that DHCP is asked again.
 *
 *   cell_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include "fakekernel.h"
#define KERNEL 1
#include <sys/board/net.h>
#include <sys/board/ipa.h>

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

/* ---- the IPA: what is sent to the modem, what the modem has for us -------------------------------------- */

static unsigned to_modem;
static uint8_t last_to_modem[1600];
static size_t last_to_modem_length;
static bool data_ready = true;
static struct { uint8_t mux; uint8_t packet[64]; size_t length; } from_modem[8];
static unsigned from_modem_n;

bool
hakios_ipa_transmit(uint8_t mux, const void *packet, size_t length)
{
	to_modem++;
	last_to_modem_length = length < sizeof(last_to_modem) ? length : sizeof(last_to_modem);
	memcpy(last_to_modem, packet, last_to_modem_length);
	return mux == 1;
}

unsigned
hakios_ipa_poll(void (*receive)(uint8_t mux, const uint8_t *packet, size_t length))
{
	unsigned n = from_modem_n;

	if (!data_ready) {
		return 0;
	}
	for (unsigned i = 0; i < n; i++) {
		receive(from_modem[i].mux, from_modem[i].packet, from_modem[i].length);
	}
	from_modem_n = 0;
	return n;
}

void hakios_ipa_status(char *to, size_t size) { snprintf(to, size, "(the test's)"); }
bool hakios_ipa_data_ready(void) { return data_ready; }

/* ---- the interface: the frames handed to the stack, the link's changes ---------------------------------- */

static unsigned inputs;
static uint8_t last_input[1600];
static size_t last_input_length;
static char link_changes[64];          /* "U" up, "D" down, in order: what the interface does of what it is told */
static unsigned link_changes_n;
static bool link_up;

void
hakios_net_input_unit(unsigned unit, const uint8_t *frame, size_t length)
{
	inputs++;
	last_input_length = length < sizeof(last_input) ? length : sizeof(last_input);
	memcpy(last_input, frame, last_input_length);
}

bool hakios_net_attach(const struct hakios_net_link *link) { return link->unit == 1; }

void
hakios_net_link_state_unit(unsigned unit, bool up)
{
	/* (as net.c: the state it has already is no change) */
	if (unit == 1 && up != link_up && link_changes_n + 1 < sizeof(link_changes)) {
		link_up = up;
		link_changes[link_changes_n++] = up ? 'U' : 'D';
	}
}

#pragma GCC diagnostic ignored "-Wformat"
#include "board/cell.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define IP(a, b, c, d)  ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static const uint8_t everyone[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t mdns_group[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb };
static const uint8_t ssdp_group[6] = { 0x01, 0x00, 0x5e, 0x7f, 0xff, 0xfa };
static const uint8_t elsewhere[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x42 };

static uint8_t frame[1514];

/* An IPv4/UDP frame from en1's own address, `length` bytes in all, zeros after the UDP header. */
static size_t
udp_frame(const uint8_t *to_mac, uint32_t from, uint32_t to, uint16_t fragment, uint16_t from_port, uint16_t to_port, size_t length)
{
	uint8_t *ip = frame + 14, *udp = ip + 20;

	memset(frame, 0, sizeof(frame));
	memcpy(frame, to_mac, 6);
	memcpy(frame + 6, cell_mac, 6);
	put16(frame + 12, 0x0800);
	ip[0] = 0x45;
	put16(ip + 2, (uint32_t)(length - 14));
	put16(ip + 6, fragment);
	ip[8] = 64; ip[9] = 17;
	put32(ip + 12, from);
	put32(ip + 16, to);
	put16(ip + 10, checksum(ip, 20));
	put16(udp, from_port);
	put16(udp + 2, to_port);
	put16(udp + 4, (uint32_t)(length - 14 - 20));
	return length;
}

/* A DHCP message of this type from the client's port to the server's, as Darwin's client sends it. */
static size_t
dhcp_frame(const uint8_t *to_mac, uint32_t from, uint32_t to, uint8_t type, uint32_t wanted)
{
	size_t length = udp_frame(to_mac, from, to, 0, 68, 67, 14 + 20 + 8 + 300);
	uint8_t *bootp = frame + 14 + 20 + 8, *o = bootp + 240;

	bootp[0] = 1; bootp[1] = 1; bootp[2] = 6;
	put32(bootp + 4, 0x12345678);
	memcpy(bootp + 28, cell_mac, 6);
	put32(bootp + 236, 0x63825363);
	*o++ = 53; *o++ = 1; *o++ = type;
	if (wanted != 0) {
		*o++ = 50; *o++ = 4; put32(o, wanted); o += 4;
	}
	*o = 255;
	return length;
}

static bool
send_frame(size_t length)
{
	struct fake_mbuf packet = { frame, length };

	return cell_transmit(&packet, length);
}

/* The DHCP message type of the last frame handed to the stack; 0 if it is not a DHCP answer. */
static unsigned
answered_with(void)
{
	const uint8_t *bootp = last_input + 14 + 20 + 8;

	if (inputs == 0 || last_input_length < 14 + 20 + 8 + 243 || last_input[23] != 17 || get32(bootp + 236) != 0x63825363 ||
	    bootp[240] != 53) {
		return 0;
	}
	return bootp[242];
}

/* en1 attached on a data path that is up, and a session with this address */
static void
start_session(uint32_t address, uint32_t dns1, uint32_t dns2, uint32_t mtu)
{
	hakios_cell_attach();
	hakios_cell_session(true, address, IP(255, 255, 255, 252), address + 1, dns1, dns2, mtu);
	CHECK(cell_attached && session.up, "en1 attached, its session up");
}

/* ---- 1: only what goes to the router goes to the modem --------------------------------------------------- */

/* mDNS and SSDP: multicast groups of the link, not for the radio */
static void
multicast_not_sent(void)
{
	start_session(IP(10, 20, 30, 40), IP(8, 8, 8, 8), 0, 1500);
	send_frame(udp_frame(mdns_group, IP(10, 20, 30, 40), IP(224, 0, 0, 251), 0, 5353, 5353, 120));
	send_frame(udp_frame(ssdp_group, IP(10, 20, 30, 40), IP(239, 255, 255, 250), 0, 50000, 1900, 200));
	/* (and a multicast group's address, even behind the router's Ethernet address) */
	send_frame(udp_frame(router_mac, IP(10, 20, 30, 40), IP(224, 0, 0, 251), 0, 5353, 5353, 120));
	CHECK(to_modem == 0, "nothing to the modem: %u packets", to_modem);
	CHECK(cell_count.dropped_out == 3, "counted as dropped: %llu", (unsigned long long)cell_count.dropped_out);
}

/* the limited broadcast (NetBIOS, a DHCP answer's way back), to everyone's Ethernet address or to the router's */
static void
broadcast_not_sent(void)
{
	start_session(IP(10, 20, 30, 40), IP(8, 8, 8, 8), 0, 1500);
	send_frame(udp_frame(everyone, IP(10, 20, 30, 40), IP(255, 255, 255, 255), 0, 137, 137, 100));
	send_frame(udp_frame(router_mac, IP(10, 20, 30, 40), IP(255, 255, 255, 255), 0, 137, 137, 100));
	CHECK(to_modem == 0, "nothing to the modem: %u packets", to_modem);
	CHECK(cell_count.dropped_out == 2, "counted as dropped: %llu", (unsigned long long)cell_count.dropped_out);
}

/* the subnet's broadcast: here a /23, since the address is a /24's broadcast address (cell_mask()) */
static void
subnet_broadcast_not_sent(void)
{
	start_session(IP(100, 64, 2, 255), IP(8, 8, 8, 8), 0, 1500);
	CHECK(session.mask == IP(255, 255, 254, 0), "a /23: %#x", session.mask);
	send_frame(udp_frame(everyone, IP(100, 64, 2, 255), IP(100, 64, 3, 255), 0, 137, 137, 100));
	send_frame(udp_frame(router_mac, IP(100, 64, 2, 255), IP(100, 64, 3, 255), 0, 137, 137, 100));
	CHECK(to_modem == 0, "nothing to the modem: %u packets", to_modem);
	CHECK(cell_count.dropped_out == 2, "counted as dropped: %llu", (unsigned long long)cell_count.dropped_out);
	/* (another network's x.y.z.255 is an address like any other: to the modem) */
	CHECK(send_frame(udp_frame(router_mac, IP(100, 64, 2, 255), IP(100, 64, 4, 255), 0, 5000, 53, 100)) && to_modem == 1,
	    "sent: %u packets", to_modem);
}

/* unicast through the router goes, the IP packet without its Ethernet header; to another station it does not */
static void
unicast_to_the_router_sent(void)
{
	size_t length;

	start_session(IP(10, 20, 30, 40), IP(8, 8, 8, 8), 0, 1500);
	length = udp_frame(router_mac, IP(10, 20, 30, 40), IP(1, 1, 1, 1), 0, 40000, 53, 80);
	CHECK(send_frame(length), "sent");
	CHECK(to_modem == 1 && cell_count.to_modem == 1, "one packet to the modem: %u", to_modem);
	CHECK(last_to_modem_length == length - 14 && memcmp(last_to_modem, frame + 14, length - 14) == 0, "the IP packet, as it was");
	CHECK(!send_frame(udp_frame(elsewhere, IP(10, 20, 30, 40), IP(1, 1, 1, 1), 0, 40000, 53, 80)), "not to another station");
	CHECK(!send_frame(udp_frame(router_mac, IP(10, 20, 30, 40), IP(1, 1, 1, 1), 0, 40000, 53, 14 + 19)), "not a cut header");
	CHECK(to_modem == 1, "still one packet to the modem: %u", to_modem);
	CHECK(cell_count.dropped_out == 2, "the others counted as dropped: %llu", (unsigned long long)cell_count.dropped_out);
}

/* ---- 2: what DHCP is ------------------------------------------------------------------------------------ */

/* a fragment that is not the first, whose bytes where a UDP header's port would be read 67: sent on, not swallowed */
static void
fragment_forwarded_not_swallowed(void)
{
	start_session(IP(10, 20, 30, 40), IP(8, 8, 8, 8), 0, 1500);
	CHECK(send_frame(udp_frame(router_mac, IP(10, 20, 30, 40), IP(8, 8, 4, 4), 185, 0, 67, 400)), "sent");
	CHECK(to_modem == 1 && inputs == 0, "to the modem, nothing answered: %u, %u", to_modem, inputs);
	/* the first fragment of a datagram to port 67 elsewhere (more fragments follow) */
	CHECK(send_frame(udp_frame(router_mac, IP(10, 20, 30, 40), IP(8, 8, 4, 4), 0x2000, 68, 67, 400)), "sent");
	CHECK(to_modem == 2 && inputs == 0, "to the modem, nothing answered: %u, %u", to_modem, inputs);
	/* and a real discover is still answered, with an offer */
	CHECK(send_frame(dhcp_frame(everyone, 0, IP(255, 255, 255, 255), DHCP_DISCOVER, 0)), "answered");
	CHECK(inputs == 1 && answered_with() == DHCP_OFFER && to_modem == 2, "an offer: %u, %u", inputs, answered_with());
}

/* UDP to port 67 of a host on the internet, from any port, even one that looks like DHCP: sent on, not answered here */
static void
port_67_elsewhere_forwarded(void)
{
	start_session(IP(10, 20, 30, 40), IP(8, 8, 8, 8), 0, 1500);
	CHECK(send_frame(udp_frame(router_mac, IP(10, 20, 30, 40), IP(198, 51, 100, 7), 0, 40000, 67, 400)), "sent");
	CHECK(send_frame(dhcp_frame(router_mac, IP(10, 20, 30, 40), IP(198, 51, 100, 7), DHCP_REQUEST, 0)), "sent");
	CHECK(to_modem == 2 && inputs == 0 && cell_count.dhcp == 0, "to the modem, nothing answered: %u, %u", to_modem, inputs);
	/* from another port to the router's 67: not the client; sent on as anything else to the router's address is */
	CHECK(send_frame(udp_frame(router_mac, IP(10, 20, 30, 40), session.router, 0, 40000, 67, 400)), "sent");
	CHECK(to_modem == 3 && inputs == 0 && cell_count.dhcp == 0, "not answered: %u, %u", to_modem, inputs);
	/* a renewal, from the address to the router: answered with an acknowledgement */
	CHECK(send_frame(dhcp_frame(router_mac, IP(10, 20, 30, 40), session.router, DHCP_REQUEST, 0)), "answered");
	CHECK(inputs == 1 && answered_with() == DHCP_ACK, "an acknowledgement: %u, %u", inputs, answered_with());
	CHECK(to_modem == 3, "nothing more to the modem: %u", to_modem);
}

/* a DHCP message not answered (a release; anything before the session) is counted, not dropped unseen, and goes nowhere */
static void
dhcp_not_answered_counted(void)
{
	hakios_cell_attach();
	CHECK(!send_frame(dhcp_frame(everyone, 0, IP(255, 255, 255, 255), DHCP_DISCOVER, 0)), "no session: not answered");
	CHECK(cell_count.dropped_out == 1, "counted: %llu", (unsigned long long)cell_count.dropped_out);
	start_session(IP(10, 20, 30, 40), IP(8, 8, 8, 8), 0, 1500);
	CHECK(!send_frame(dhcp_frame(router_mac, IP(10, 20, 30, 40), session.router, 7, 0)), "a release: not answered");
	CHECK(cell_count.dropped_out == 2, "counted: %llu", (unsigned long long)cell_count.dropped_out);
	CHECK(to_modem == 0 && inputs == 0, "nothing to the modem nor to the stack: %u, %u", to_modem, inputs);
}

/* ---- 3: the data path stopped ----------------------------------------------------------------------------- */

/* stopped at the run's end with the modem kept (nobody says the session is over): en1's link goes down, and stays down */
static void
data_path_stopped(void)
{
	start_session(IP(10, 20, 30, 40), IP(8, 8, 8, 8), 0, 1500);
	cell_poll();
	CHECK(strcmp(link_changes, "U") == 0, "up, and kept up while the data path is: %s", link_changes);
	data_ready = false;
	cell_poll();
	CHECK(strcmp(link_changes, "UD") == 0, "taken down: %s", link_changes);
	CHECK(!session.up, "the session is over for en1");
	CHECK(!send_frame(dhcp_frame(everyone, 0, IP(255, 255, 255, 255), DHCP_DISCOVER, 0)) && inputs == 0, "DHCP not answered");
	CHECK(!send_frame(udp_frame(router_mac, IP(10, 20, 30, 40), IP(1, 1, 1, 1), 0, 40000, 53, 80)) && to_modem == 0,
	    "nothing to the modem");
	cell_poll();
	cell_poll();
	CHECK(strcmp(link_changes, "UD") == 0, "said once: %s", link_changes);
	/* the data path again, and a session on it: up again */
	data_ready = true;
	hakios_cell_session(true, IP(10, 20, 30, 40), 0, 0, IP(8, 8, 8, 8), 0, 1500);
	cell_poll();
	CHECK(strcmp(link_changes, "UDU") == 0, "up again: %s", link_changes);
}

/* ---- 4: the session's mux only ---------------------------------------------------------------------------- */

static void
other_mux_dropped(void)
{
	static const uint8_t packet[28] = { 0x45, 0, 0, 28, 0, 0, 0, 0, 64, 17, 0, 0, 1, 1, 1, 1, 10, 20, 30, 40 };

	start_session(IP(10, 20, 30, 40), IP(8, 8, 8, 8), 0, 1500);
	for (unsigned i = 0; i < 3; i++) {
		from_modem[i].mux = i == 1 ? CELL_MUX : (uint8_t)(i == 0 ? 0 : 2);
		memcpy(from_modem[i].packet, packet, sizeof(packet));
		from_modem[i].length = sizeof(packet);
	}
	from_modem_n = 3;
	cell_poll();
	CHECK(inputs == 1 && cell_count.from_modem == 1, "the packet on mux 1 only: %u", inputs);
	CHECK(cell_count.dropped_in == 2, "the others counted as dropped: %llu", (unsigned long long)cell_count.dropped_in);
	CHECK(last_input_length == 14 + sizeof(packet) && memcmp(last_input, cell_mac, 6) == 0 && memcmp(last_input + 6, router_mac, 6) == 0 &&
	    memcmp(last_input + 14, packet, sizeof(packet)) == 0, "from the router to en1, as it came");
}

/* ---- 5: a new session's settings ------------------------------------------------------------------------------ */

static void
new_session(uint32_t dns1, uint32_t dns2, uint32_t mtu, const char *wanted)
{
	start_session(IP(10, 20, 30, 40), IP(8, 8, 8, 8), IP(8, 8, 4, 4), 1500);
	/* the same again: nothing */
	hakios_cell_session(true, IP(10, 20, 30, 40), 0, 0, IP(8, 8, 8, 8), IP(8, 8, 4, 4), 1500);
	CHECK(strcmp(link_changes, "U") == 0, "the same session: no bounce: %s", link_changes);
	hakios_cell_session(true, IP(10, 20, 30, 40), 0, 0, dns1, dns2, mtu);
	CHECK(strcmp(link_changes, wanted) == 0, "%s, wanted %s", link_changes, wanted);
	/* and DHCP gives out the new settings */
	send_frame(dhcp_frame(everyone, 0, IP(255, 255, 255, 255), DHCP_DISCOVER, 0));
	CHECK(answered_with() == DHCP_OFFER && session.dns1 == dns1 && session.dns2 == dns2 && session.mtu == mtu, "an offer");
}

static void new_dns_bounces_the_link(void) { new_session(IP(1, 1, 1, 1), IP(8, 8, 4, 4), 1500, "UDU"); }
static void new_second_dns_bounces_the_link(void) { new_session(IP(8, 8, 8, 8), 0, 1500, "UDU"); }
static void new_mtu_bounces_the_link(void) { new_session(IP(8, 8, 8, 8), IP(8, 8, 4, 4), 1400, "UDU"); }

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "multicast_not_sent", multicast_not_sent }, { "broadcast_not_sent", broadcast_not_sent },
		{ "subnet_broadcast_not_sent", subnet_broadcast_not_sent }, { "unicast_to_the_router_sent", unicast_to_the_router_sent },
		{ "fragment_forwarded_not_swallowed", fragment_forwarded_not_swallowed },
		{ "port_67_elsewhere_forwarded", port_67_elsewhere_forwarded }, { "dhcp_not_answered_counted", dhcp_not_answered_counted },
		{ "data_path_stopped", data_path_stopped }, { "other_mux_dropped", other_mux_dropped },
		{ "new_dns_bounces_the_link", new_dns_bounces_the_link }, { "new_second_dns_bounces_the_link", new_second_dns_bounces_the_link },
		{ "new_mtu_bounces_the_link", new_mtu_bounces_the_link },
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
			fprintf(stderr, "%-34s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
