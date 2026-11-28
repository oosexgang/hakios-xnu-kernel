/* Host test of drivers/modem/hakios.loc.c: the integer conversions, the LOC frames, and the client's request order. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/hakios.loc.h>
#include <sys/hakios.qrtr.h>

static unsigned char frame[512];
static size_t frame_length;

static void
begin(unsigned char type, unsigned short message)
{
	frame[0] = type; frame[1] = 7; frame[2] = 0; frame[3] = message & 0xff; frame[4] = message >> 8;
	frame_length = 7;
}

static void
tlv(unsigned char type, const void *value, unsigned short size)
{
	frame[frame_length] = type; frame[frame_length + 1] = size & 0xff; frame[frame_length + 2] = size >> 8;
	memcpy(frame + frame_length + 3, value, size);
	frame_length += 3 + size;
	frame[5] = (frame_length - 7) & 0xff; frame[6] = (frame_length - 7) >> 8;
}

static void tlv_double(unsigned char type, double v) { tlv(type, &v, 8); }
static void tlv_float(unsigned char type, float v) { tlv(type, &v, 4); }
static void tlv_u32(unsigned char type, unsigned v) { tlv(type, &v, 4); }

static int64_t
d(double v, unsigned scale)
{
	uint64_t bits; int64_t out = 0;
	memcpy(&bits, &v, 8);
	assert(hakios_loc_double_scaled(bits, scale, &out));
	return out;
}

static int64_t
f(float v, unsigned scale)
{
	uint32_t bits; int64_t out = 0;
	memcpy(&bits, &v, 4);
	assert(hakios_loc_float_scaled(bits, scale, &out));
	return out;
}

/* the fake transport */
static hakios_qrtr_receive_t received;
static hakios_qrtr_server_t server;
static void *context;
static unsigned short sent[16];
static unsigned sent_count;

static struct hakios_qrtr_port *open_port(hakios_qrtr_receive_t r, hakios_qrtr_server_t s, void *c)
{ received = r; server = s; context = c; return (struct hakios_qrtr_port *)(uintptr_t)1; }
static int send_packet(struct hakios_qrtr_port *p, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const unsigned char *b = data;
	(void)p; assert(node == 0 && port == 33); assert(length >= 7 && b[0] == 0);
	assert((size_t)(b[5] | b[6] << 8) == length - 7);
	sent[sent_count++] = (unsigned short)(b[3] | b[4] << 8);
	if (b[3] == 0x21) {     /* REG_EVENTS: the mask, then "MHAL", type 1 (AFW), no position-request notifications */
		static const unsigned char who[] = { 0x10, 4, 0, 'M', 'H', 'A', 'L', 0x11, 4, 0, 1, 0, 0, 0, 0x12, 1, 0, 0 };

		assert(length == 7 + 11 + sizeof(who) && b[7] == 0x01 && memcmp(b + 7 + 11, who, sizeof(who)) == 0);
	}
	return 0;
}
static int lookup(struct hakios_qrtr_port *p, uint32_t s, uint32_t v, uint32_t i) { (void)p; assert(s == 0x10); (void)v; (void)i; return 0; }
static void close_port(struct hakios_qrtr_port *p) { (void)p; }
static const struct hakios_qrtr_ops ops = { open_port, send_packet, lookup, close_port };

static void
answer(unsigned short message, unsigned short result)
{
	unsigned char r[4] = { result & 0xff, result >> 8, 0, 0 };
	begin(2, message);
	tlv(2, r, 4);
	received(context, 0, 33, frame, frame_length);
}

int
main(void)
{
	struct hakios_gnss_fix fix = { .speed_mms = -1, .heading_mdeg = -1 };
	struct hakios_gnss_sky sky = { 0 };
	uint32_t state = HAKIOS_GNSS_SESSION, result = 0;
	uint16_t response = 0;

	/* conversions */
	assert(d(-33.8688197, 10000000) == -338688197);
	assert(d(151.2092955, 10000000) == 1512092955);
	assert(d(0.0, 10000000) == 0);
	assert(d(89.9999999, 10000000) == 899999999);
	assert(f(4.25f, 1000) == 4250);
	assert(f(-12.5f, 1000) == -12500);
	assert(f(359.5f, 1000) == 359500);
	assert(f(0.001f, 1000) == 1);

	/* a position report with everything */
	begin(4, 0x24);
	tlv_u32(0x01, 0);
	tlv_double(0x10, -33.8688197);
	tlv_double(0x11, 151.2092955);
	tlv_float(0x12, 4.5f);
	tlv_float(0x18, 1.25f);
	tlv_float(0x1a, 60.0f);
	tlv_float(0x1b, 38.0f);
	tlv_float(0x1c, 7.0f);
	tlv_float(0x20, 270.0f);
	{ uint64_t t = 1794500000123ull; tlv(0x25, &t, 8); }
	{ unsigned char used[1 + 2 * 3] = { 3, 1, 0, 2, 0, 3, 0 }; tlv(0x2c, used, sizeof(used)); }
	assert(hakios_loc_apply(&fix, &sky, &state, frame, frame_length, &response, &result) == 1);
	assert(response == 0 && state == HAKIOS_GNSS_FIX && fix.seq == 1);
	assert(fix.flags == (HAKIOS_GNSS_HAS_POSITION | HAKIOS_GNSS_HAS_ALTITUDE | HAKIOS_GNSS_FINAL));
	assert(fix.lat_e7 == -338688197 && fix.lon_e7 == 1512092955);
	assert(fix.hacc_mm == 4500 && fix.vacc_mm == 7000 && fix.alt_mm == 38000);
	assert(fix.speed_mms == 1250 && fix.heading_mdeg == 270000);
	assert(fix.utc_ms == 1794500000123ull && fix.sats_used == 3);

	/* in progress, no position: the fix is lost, the session goes on */
	begin(4, 0x24);
	tlv_u32(0x01, 1);
	assert(hakios_loc_apply(&fix, &sky, &state, frame, frame_length, &response, &result) == 1);
	assert(state == HAKIOS_GNSS_SESSION && fix.seq == 2 && !(fix.flags & HAKIOS_GNSS_HAS_POSITION));

	/* satellites: two, one tracked at 41 dB-Hz */
	{
		unsigned char list[1 + 2 * 28] = { 2 };
		uint32_t valid = 0xff, status_track = 3, status_search = 2;
		float snr = 41.0f, low = 12.0f;

		memcpy(list + 1, &valid, 4); memcpy(list + 1 + 11, &status_track, 4); memcpy(list + 1 + 24, &snr, 4);
		memcpy(list + 29, &valid, 4); memcpy(list + 29 + 11, &status_search, 4); memcpy(list + 29 + 24, &low, 4);
		begin(4, 0x25);
		{ unsigned char assumed = 0; tlv(0x01, &assumed, 1); }
		tlv(0x10, list, sizeof(list));
	}
	assert(hakios_loc_apply(&fix, &sky, &state, frame, frame_length, &response, &result) == 1);
	assert(sky.seq == 1 && sky.seen == 2 && sky.tracked == 1 && sky.best_snr_db == 41);

	/* a malformed frame */
	begin(4, 0x24);
	tlv_u32(0x01, 0);
	frame[5] = 99;
	assert(hakios_loc_apply(&fix, &sky, &state, frame, frame_length, &response, &result) == -1);

	/* the client: revision, events, mode, start, in that order, each after the answer to the one before */
	{
		struct hakios_loc_client client;

		assert(hakios_loc_client_open(&client, &ops, 4, 1000, NULL, NULL) == 0);
		server(context, true, 0x10, 2, 0, 0, 33);
		assert(client.state == HAKIOS_GNSS_SERVICE && sent_count == 1 && sent[0] == 0x20);
		answer(0x20, 0);
		assert(sent_count == 2 && sent[1] == 0x21);
		answer(0x21, 0);
		assert(client.state == HAKIOS_GNSS_REGISTERED && sent_count == 3 && sent[2] == 0x3b);
		answer(0x3b, 0);
		{       /* the GET_ENGINE_LOCK indication: success, lock 4 (all) */
			unsigned char ok[4] = { 0 }, all[4] = { 4, 0, 0, 0 };

			begin(4, 0x3b);
			tlv(0x01, ok, 4);
			tlv(0x10, all, 4);
			received(context, 0, 33, frame, frame_length);
			assert(client.lock_found == 4);
		}
		assert(sent_count == 4 && sent[3] == 0x4a);
		answer(0x4a, 1);        /* the mode refused: Start is asked all the same */
		assert(client.refused == 1 && client.refused_message[0] == 0x4a && sent_count == 5 && sent[4] == 0x22);
		answer(0x22, 0);
		assert(client.state == HAKIOS_GNSS_SESSION && sent_count == 5);
		hakios_loc_client_close(&client);
		assert(sent_count == 6 && sent[5] == 0x23);
	}
	printf("test_loc: all passed\n");
	return 0;
}
