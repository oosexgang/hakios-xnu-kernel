/*
 * GNSS through the modem's location service: QMI LOC (service 0x10) over QRTR, as sys/hakios.loc.h says.
 *
 * Wire numbers are those of Qualcomm's public LOC interface (the QMI_LOC_* messages that libqmi's LOC service and
 * Android's location HAL also speak); nothing here is taken from either's code. The client:
 *   0x0020 Inform Client Revision   (our revision, 2)
 *   0x0021 Register Events          (position, satellites, engine state, fix session state)
 *   0x004a Set Operation Mode       (standalone by default: no assistance server is reachable from here)
 *   0x0022 Start                    (session 1, periodic, high accuracy, intermediate reports, every interval_ms)
 * each sent when the one before has been answered, and then listens:
 *   0x0024 Position Report          0x01 session status, 0x10/0x11 latitude/longitude (double), 0x12 horizontal
 *                                   uncertainty (float, m), 0x18 horizontal speed (float, m/s), 0x1a/0x1b altitude
 *                                   above the ellipsoid/sea level (float, m), 0x1c vertical uncertainty, 0x20 heading
 *                                   (float, degrees), 0x25 UTC time (uint64, ms), 0x2c satellites used (count + ids)
 *   0x0025 GNSS SV Info             0x10 the satellites: a count and 28 bytes each (valid mask, system, id, health,
 *                                   status 1 idle 2 searching 3 tracking, info mask, elevation, azimuth, SNR)
 *   0x002b Engine State             0x01 1 on, 2 off
 *   0x002c Fix Session State        0x01 1 started, 2 finished
 *   0x004a Set Operation Mode       0x01 its status, as an indication after the response
 * It only listens to the GNSS: no assistance data is injected and nothing is written to the modem's settings.
 */
#include <sys/hakios.loc.h>
#include <sys/hakios.qrtr.h>
#include <sys/systm.h>

#define LOC_INFORM_CLIENT_REVISION      0x0020
#define LOC_REG_EVENTS                  0x0021
#define LOC_START                       0x0022
#define LOC_STOP                        0x0023
#define LOC_POSITION_REPORT             0x0024
#define LOC_GNSS_SV_INFO                0x0025
#define LOC_ENGINE_STATE                0x002b
#define LOC_FIX_SESSION_STATE           0x002c
#define LOC_SET_OPERATION_MODE          0x004a
#define LOC_SET_ENGINE_LOCK             0x003a  /* QMI LOC v02: lock type 0x01 (1 none, 2 mobile-initiated, 3 MT, 4 all) */
#define LOC_GET_ENGINE_LOCK             0x003b  /* its indication: status 0x01, lock type 0x10 */
#define LOC_CLIENT_AFW                  1       /* REG_EVENTS' client type: an application framework client */

#define LOC_EVENT_POSITION_REPORT       0x001ull
#define LOC_EVENT_GNSS_SV_INFO          0x002ull
#define LOC_EVENT_ENGINE_STATE          0x080ull
#define LOC_EVENT_FIX_SESSION_STATE     0x100ull

#define LOC_SESSION_ID                  1
#define LOC_SV_ENTRY                    28

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

/*
 * value * scale from IEEE 754 bits without the FPU. The mantissa is cut to its top 32 bits first (a relative error
 * of 2^-32: 2 * 10^-8 degrees at 90 degrees, well inside 10^-7), so that the product fits 64 bits.
 */
bool
hakios_loc_double_scaled(uint64_t bits, uint32_t scale, int64_t *out)
{
	int exponent = (int)((bits >> 52) & 0x7ff);
	uint64_t mantissa = bits & ((1ull << 52) - 1), value;
	int shift;

	if (exponent == 0x7ff) {
		return false;
	}
	if (exponent == 0) {            /* zero, or too small to matter */
		*out = 0;
		return true;
	}
	mantissa = (mantissa | 1ull << 52) >> 21;       /* 32 bits */
	value = mantissa * scale;                       /* < 2^64 for any scale < 2^32 */
	shift = exponent - 1075 + 21;
	if (shift > 0) {
		if (shift > 30 || value > (UINT64_MAX >> 1) >> shift) {
			return false;
		}
		value <<= shift;
	} else if (shift < 0) {
		value = shift <= -64 ? 0 : (value + (1ull << (-shift - 1))) >> -shift;
	}
	if (value > (uint64_t)INT64_MAX) {
		return false;
	}
	*out = (bits >> 63) ? -(int64_t)value : (int64_t)value;
	return true;
}

bool
hakios_loc_float_scaled(uint32_t bits, uint32_t scale, int64_t *out)
{
	int exponent = (int)((bits >> 23) & 0xff);
	uint64_t mantissa = bits & 0x7fffff;

	if (exponent == 0xff) {
		return false;
	}
	if (exponent == 0) {
		*out = 0;
		return true;
	}
	/* the same number as a double */
	return hakios_loc_double_scaled((uint64_t)(bits >> 31) << 63 | (uint64_t)(exponent - 127 + 1023) << 52 | mantissa << 29,
	           scale, out);
}

static bool
float_tlv(const uint8_t *value, uint16_t size, uint32_t scale, int64_t *out)
{
	return size == 4 && hakios_loc_float_scaled(le32(value), scale, out);
}

int
hakios_loc_apply(struct hakios_gnss_fix *fix, struct hakios_gnss_sky *sky, uint32_t *state, const void *frame,
    size_t length, uint16_t *response, uint32_t *result)
{
	const uint8_t *data = frame;
	struct hakios_gnss_fix next;
	struct hakios_gnss_sky sky_next;
	uint16_t message, payload_size;
	bool lat = false, lon = false, msl = false, ellipsoid = false, result_seen = false;
	int64_t v, latitude = 0, longitude = 0, altitude_msl = 0, altitude_ellipsoid = 0;
	size_t at;

	if (fix == NULL || sky == NULL || state == NULL || data == NULL || length < 7) {
		return -1;
	}
	if (response != NULL) {
		*response = 0;
	}
	if (data[0] != 2 && data[0] != 4) {
		return 0;
	}
	message = le16(data + 3);
	payload_size = le16(data + 5);
	if ((size_t)payload_size != length - 7) {
		return -1;
	}
	next = *fix;
	sky_next = *sky;
	if (data[0] == 4 && message == LOC_POSITION_REPORT) {
		next.flags &= HAKIOS_GNSS_FAKE;
		next.hacc_mm = next.vacc_mm = 0;
		next.speed_mms = next.heading_mdeg = -1;
		next.sats_used = 0;
	}
	for (at = 7; at < length;) {
		uint8_t type;
		uint16_t size;
		const uint8_t *value;

		if (length - at < 3) {
			return -1;
		}
		type = data[at];
		size = le16(data + at + 1);
		at += 3;
		if (size > length - at) {
			return -1;
		}
		value = data + at;
		at += size;
		if (data[0] == 2) {
			if (type == 2 && size == 4) {
				result_seen = true;
				if (result != NULL) {
					*result = le16(value) | (uint32_t)le16(value + 2) << 16;
				}
			}
			continue;
		}
		switch (message) {
		case LOC_POSITION_REPORT:
			switch (type) {
			case 0x01:
				if (size == 4) {
					next.status = le32(value);
				}
				break;
			case 0x10:
				lat = size == 8 && hakios_loc_double_scaled(le64(value), 10000000, &latitude) &&
				    latitude >= -900000000 && latitude <= 900000000;
				break;
			case 0x11:
				lon = size == 8 && hakios_loc_double_scaled(le64(value), 10000000, &longitude) &&
				    longitude >= -1800000000 && longitude <= 1800000000;
				break;
			case 0x12:
				if (float_tlv(value, size, 1000, &v) && v >= 0 && v < UINT32_MAX) {
					next.hacc_mm = (uint32_t)v;
				}
				break;
			case 0x18:
				if (float_tlv(value, size, 1000, &v) && v >= 0 && v < INT32_MAX) {
					next.speed_mms = (int32_t)v;
				}
				break;
			case 0x1a:
				ellipsoid = float_tlv(value, size, 1000, &altitude_ellipsoid) && altitude_ellipsoid > -2000000 &&
				    altitude_ellipsoid < 100000000;
				break;
			case 0x1b:
				msl = float_tlv(value, size, 1000, &altitude_msl) && altitude_msl > -2000000 && altitude_msl < 100000000;
				break;
			case 0x1c:
				if (float_tlv(value, size, 1000, &v) && v >= 0 && v < UINT32_MAX) {
					next.vacc_mm = (uint32_t)v;
				}
				break;
			case 0x20:
				if (float_tlv(value, size, 1000, &v) && v >= 0 && v < 360000) {
					next.heading_mdeg = (int32_t)v;
				}
				break;
			case 0x25:
				if (size == 8) {
					next.utc_ms = le64(value);
				}
				break;
			case 0x2c:
				if (size >= 1 && size >= 1 + 2 * (size_t)value[0]) {
					next.sats_used = value[0];
				}
				break;
			}
			break;
		case LOC_GNSS_SV_INFO:
			if (type == 0x10 && size >= 1) {
				unsigned count = value[0], tracked = 0, best = 0;

				if ((size_t)size == 1 + (size_t)count * LOC_SV_ENTRY) {
					for (unsigned i = 0; i < count; i++) {
						const uint8_t *sv = value + 1 + i * LOC_SV_ENTRY;
						uint32_t valid = le32(sv), status = le32(sv + 11);
						int64_t snr = 0;

						if ((valid & 0x08) && status == 3) {
							tracked++;
						}
						if ((valid & 0x80) && hakios_loc_float_scaled(le32(sv + 24), 1, &snr) && snr > (int64_t)best &&
						    snr < 100) {
							best = (unsigned)snr;
						}
					}
				}
				sky_next.seen = count;
				sky_next.tracked = tracked;
				sky_next.best_snr_db = best;
				sky_next.seq++;
			}
			break;
		case LOC_ENGINE_STATE:
			if (type == 0x01 && size == 4) {
				sky_next.engine = le32(value);
			}
			break;
		}
	}
	if (data[0] == 2) {
		if (response != NULL) {
			*response = message;
		}
		return result_seen ? 0 : -1;
	}
	if (message == LOC_POSITION_REPORT) {
		next.seq++;
		if (lat && lon) {
			next.flags |= HAKIOS_GNSS_HAS_POSITION;
			next.lat_e7 = (int32_t)latitude;
			next.lon_e7 = (int32_t)longitude;
			if (next.status == 0) {
				next.flags |= HAKIOS_GNSS_FINAL;
			}
			*state = HAKIOS_GNSS_FIX;
		} else if (*state > HAKIOS_GNSS_SESSION) {
			*state = HAKIOS_GNSS_SESSION;
		}
		if (msl || ellipsoid) {
			next.flags |= HAKIOS_GNSS_HAS_ALTITUDE;
			next.alt_mm = (int32_t)(msl ? altitude_msl : altitude_ellipsoid);
		}
		*fix = next;
		return 1;
	}
	if (message == LOC_GNSS_SV_INFO || message == LOC_ENGINE_STATE) {
		bool changed = sky_next.seq != sky->seq || sky_next.engine != sky->engine;

		*sky = sky_next;
		return changed ? 1 : 0;
	}
	return 0;
}

/* ---- the client ------------------------------------------------------------------------------------------- */

static const char *
status_name(uint32_t status)
{
	static const char *const names[] = { "success", "in progress", "general failure", "timeout", "ended by user",
		                             "bad parameter", "phone offline", "engine locked" };

	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "?";
}

static int
loc_send(struct hakios_loc_client *client, uint16_t message, const uint8_t *tlvs, size_t tlv_length)
{
	uint8_t frame[64];

	if (client->port == NULL || !client->connected || 7 + tlv_length > sizeof(frame)) {
		return -1;
	}
	if (++client->transaction == 0) {
		client->transaction = 1;
	}
	frame[0] = 0;                   /* request */
	frame[1] = (uint8_t)client->transaction;
	frame[2] = (uint8_t)(client->transaction >> 8);
	frame[3] = (uint8_t)message;
	frame[4] = (uint8_t)(message >> 8);
	frame[5] = (uint8_t)tlv_length;
	frame[6] = (uint8_t)(tlv_length >> 8);
	memcpy(frame + 7, tlvs, tlv_length);
	client->asked = (uint8_t)message;
	return client->transport->send(client->port, client->peer_node, client->peer_port, frame, 7 + tlv_length);
}

static size_t
tlv32(uint8_t *at, uint8_t type, uint32_t value)
{
	at[0] = type;
	at[1] = 4;
	at[2] = 0;
	at[3] = (uint8_t)value;
	at[4] = (uint8_t)(value >> 8);
	at[5] = (uint8_t)(value >> 16);
	at[6] = (uint8_t)(value >> 24);
	return 7;
}

/* the next request after `answered` (0: the first) */
static void
loc_advance(struct hakios_loc_client *client, uint16_t answered)
{
	uint8_t tlvs[48];
	size_t n = 0;
	int error = 0;

	switch (answered) {
	case 0:
		n = tlv32(tlvs, 0x01, 2);
		error = loc_send(client, LOC_INFORM_CLIENT_REVISION, tlvs, n);
		break;
	case LOC_INFORM_CLIENT_REVISION: {
		uint64_t mask = LOC_EVENT_POSITION_REPORT | LOC_EVENT_GNSS_SV_INFO | LOC_EVENT_ENGINE_STATE |
		    LOC_EVENT_FIX_SESSION_STATE;

		tlvs[0] = 0x01;
		tlvs[1] = 8;
		tlvs[2] = 0;
		for (unsigned i = 0; i < 8; i++) {
			tlvs[3 + i] = (uint8_t)(mask >> (8 * i));
		}
		n = 11;
		/*
		 * Who we are, as newer modems want it (sm8450's refused the mask alone: QMI error 48, invalid argument):
		 * the client's name (0x10, at most 4 characters, no terminator), "MHAL" as Qualcomm's location HAL names its
		 * master client, of which we are the only one; its type (0x11) 1, an application framework client; and no
		 * notifications of others' position requests (0x12).
		 */
		tlvs[n++] = 0x10;
		tlvs[n++] = 4;
		tlvs[n++] = 0;
		memcpy(tlvs + n, "MHAL", 4);
		n += 4;
		n += tlv32(tlvs + n, 0x11, LOC_CLIENT_AFW);
		tlvs[n++] = 0x12;
		tlvs[n++] = 1;
		tlvs[n++] = 0;
		tlvs[n++] = 0;
		error = loc_send(client, LOC_REG_EVENTS, tlvs, n);
		break;
	}
	/*
	 * The engine's lock, read for the log: position reports with session status 7 (QMI LOC v02's
	 * eQMI_LOC_SESS_STATUS_ENGINE_LOCKED) say sessions like ours are locked out. On zeus (run 041430) the lock read
	 * 3, mobile-terminated sessions only, so ours were not locked by it; and setting it is refused to us (QMI error
	 * 67, session ownership), so it is not set.
	 */
	case LOC_REG_EVENTS:
		error = loc_send(client, LOC_GET_ENGINE_LOCK, tlvs, 0);
		break;
	case LOC_GET_ENGINE_LOCK:
		n = tlv32(tlvs, 0x01, client->mode);
		error = loc_send(client, LOC_SET_OPERATION_MODE, tlvs, n);
		break;
	case LOC_SET_OPERATION_MODE:
		tlvs[0] = 0x01;                         /* session id */
		tlvs[1] = 1;
		tlvs[2] = 0;
		tlvs[3] = LOC_SESSION_ID;
		n = 4;
		n += tlv32(tlvs + n, 0x10, 1);          /* periodic */
		n += tlv32(tlvs + n, 0x11, 3);          /* high accuracy */
		n += tlv32(tlvs + n, 0x12, 1);          /* intermediate reports on: positions before the final one */
		n += tlv32(tlvs + n, 0x13, client->interval_ms);
		error = loc_send(client, LOC_START, tlvs, n);
		break;
	default:
		return;
	}
	if (error != 0) {
		printf("gnss: request %#x could not be sent (%d)\n", client->asked, error);
	}
}

static void
loc_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct hakios_loc_client *client = context;
	const uint8_t *bytes = data;
	uint16_t response = 0;
	uint32_t result = 0, before = client->state;
	struct hakios_gnss_fix was = client->fix;
	int changed;

	if (!client->connected || node != client->peer_node || port != client->peer_port) {
		return;
	}
	client->frames++;
	changed = hakios_loc_apply(&client->fix, &client->sky, &client->state, data, length, &response, &result);
	if (changed < 0) {
		printf("gnss: a malformed frame of %zu bytes (message %#x)\n", length, length >= 5 ? le16(bytes + 3) : 0);
		return;
	}
	if (response != 0) {
		if ((result & 0xffff) != 0) {
			if (client->refused < sizeof(client->refused_message) / sizeof(client->refused_message[0])) {
				client->refused_message[client->refused] = response;
				client->refused_error[client->refused] = (uint16_t)(result >> 16);
			}
			client->refused++;
			printf("gnss: the modem refused request %#x: error %u\n", response, result >> 16);
		}
		if (response == LOC_REG_EVENTS && (result & 0xffff) == 0 && client->state < HAKIOS_GNSS_REGISTERED) {
			client->state = HAKIOS_GNSS_REGISTERED;
			printf("gnss: events registered (position, satellites, engine, session)\n");
		}
		if (response == LOC_START && (result & 0xffff) == 0) {
			if (client->state < HAKIOS_GNSS_SESSION) {
				client->state = HAKIOS_GNSS_SESSION;
			}
			printf("gnss: SESSION STARTED (mode %u, a fix every %u ms); waiting for satellites\n", client->mode,
			    client->interval_ms);
		}
		/* a refusal of the revision, the lock or the mode is not the end: Start is still asked */
		loc_advance(client, response);
		changed = 1;
	} else if (length >= 7 && bytes[0] == 4) {
		uint16_t message = le16(bytes + 3);

		if (message == LOC_POSITION_REPORT) {
			const struct hakios_gnss_fix *f = &client->fix;
			static unsigned shown;

			if ((f->flags & HAKIOS_GNSS_HAS_POSITION) && (!(was.flags & HAKIOS_GNSS_HAS_POSITION) || shown < 10 ||
			    f->seq % 60 == 0)) {
				shown++;
				printf("gnss: FIX #%u: %d.%07u %d.%07u, +-%u.%03u m, alt %d mm, %d mm/s, heading %d mdeg, %u sats, utc %llu ms "
				    "(%s)\n", f->seq, f->lat_e7 / 10000000, (unsigned)(f->lat_e7 < 0 ? -f->lat_e7 : f->lat_e7) % 10000000,
				    f->lon_e7 / 10000000, (unsigned)(f->lon_e7 < 0 ? -f->lon_e7 : f->lon_e7) % 10000000, f->hacc_mm / 1000,
				    f->hacc_mm % 1000, f->alt_mm, f->speed_mms, f->heading_mdeg, f->sats_used, (unsigned long long)f->utc_ms,
				    status_name(f->status));
			} else if (!(f->flags & HAKIOS_GNSS_HAS_POSITION) && (f->seq <= 5 || f->seq % 30 == 0)) {
				printf("gnss: position report #%u without a position: %s; satellites seen %u, tracked %u, best %u dB-Hz\n",
				    f->seq, status_name(f->status), client->sky.seen, client->sky.tracked, client->sky.best_snr_db);
			}
		} else if (message == LOC_GNSS_SV_INFO) {
			if (client->sky.seq <= 3 || client->sky.seq % 30 == 0) {
				printf("gnss: satellites #%u: %u seen, %u tracked, best %u dB-Hz\n", client->sky.seq, client->sky.seen,
				    client->sky.tracked, client->sky.best_snr_db);
			}
		} else if (message == LOC_ENGINE_STATE) {
			printf("gnss: engine %s\n", client->sky.engine == 1 ? "ON" : client->sky.engine == 2 ? "off" : "?");
		} else if (message == LOC_FIX_SESSION_STATE && length >= 14 && bytes[7] == 1) {
			printf("gnss: fix session %s\n", le32(bytes + 10) == 1 ? "started" : le32(bytes + 10) == 2 ? "finished" : "?");
		} else if (message == LOC_SET_OPERATION_MODE && length >= 14 && bytes[7] == 1) {
			client->mode_status = le32(bytes + 10) + 1;
			printf("gnss: operation mode %u: %s\n", client->mode, status_name(le32(bytes + 10)));
		} else if (message == LOC_SET_ENGINE_LOCK && length >= 14 && bytes[7] == 1) {
			client->lock_status = le32(bytes + 10) + 1;
			printf("gnss: engine lock set: status %u (0 success)\n", le32(bytes + 10));
		} else if (message == LOC_GET_ENGINE_LOCK) {
			/* TLVs from byte 7: the status (0x01), then the lock type (0x10) */
			for (size_t at = 7; at + 3 <= length;) {
				uint16_t size = le16(bytes + at + 1);

				if (at + 3 + size > length) {
					break;
				}
				if (bytes[at] == 0x10 && size >= 4) {
					client->lock_found = le32(bytes + at + 3);
					printf("gnss: the engine's lock was %u (1 none, 2 MI, 3 MT, 4 all)\n", client->lock_found);
				}
				at += 3 + size;
			}
		}
	}
	if (client->state != before) {
		printf("gnss: state %u -> %u\n", before, client->state);
	}
	if (changed > 0 && client->changed != NULL) {
		client->changed(client->context, client);
	}
}

static void
loc_server(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance, uint32_t node, uint32_t port)
{
	struct hakios_loc_client *client = context;

	if (service != HAKIOS_LOC_SERVICE) {
		return;
	}
	if (arrived && !client->connected) {
		client->connected = true;
		client->peer_node = node;
		client->peer_port = port;
		client->state = HAKIOS_GNSS_SERVICE;
		printf("gnss: the modem's LOCATION SERVICE (QMI %#x, version %u, instance %u) at node %u port %u\n", service, version,
		    instance, node, port);
		loc_advance(client, 0);
		if (client->changed != NULL) {
			client->changed(client->context, client);
		}
	} else if (!arrived && client->connected && node == client->peer_node && port == client->peer_port) {
		client->connected = false;
		client->state = HAKIOS_GNSS_NONE;
		client->fix.flags = 0;
		printf("gnss: the location service has gone\n");
		if (client->changed != NULL) {
			client->changed(client->context, client);
		}
	}
}

int
hakios_loc_client_open(struct hakios_loc_client *client, const struct hakios_qrtr_ops *transport, uint32_t mode,
    uint32_t interval_ms, void (*changed)(void *, const struct hakios_loc_client *), void *context)
{
	int error;

	if (client == NULL || transport == NULL || transport->open == NULL || transport->lookup == NULL ||
	    transport->send == NULL || transport->close == NULL) {
		return -1;
	}
	*client = (struct hakios_loc_client){0};
	client->transport = transport;
	client->mode = mode != 0 ? mode : 4;
	client->interval_ms = interval_ms >= 100 ? interval_ms : 1000;
	client->changed = changed;
	client->context = context;
	client->fix.speed_mms = client->fix.heading_mdeg = -1;
	client->port = transport->open(loc_receive, loc_server, client);
	if (client->port == NULL) {
		return -1;
	}
	error = transport->lookup(client->port, HAKIOS_LOC_SERVICE, 0, 0);
	if (error != 0) {
		hakios_loc_client_close(client);
		return error;
	}
	return 0;
}

void
hakios_loc_client_close(struct hakios_loc_client *client)
{
	if (client == NULL || client->port == NULL) {
		return;
	}
	if (client->connected && client->state >= HAKIOS_GNSS_SESSION) {
		uint8_t tlv[4] = { 0x01, 1, 0, LOC_SESSION_ID };

		(void)loc_send(client, LOC_STOP, tlv, sizeof(tlv));
	}
	client->transport->close(client->port);
	client->port = NULL;
	client->connected = false;
	client->state = HAKIOS_GNSS_NONE;
}

/* ---- kern.hakios.gnss_*: the kernel's copy, for userland (locationd, through libhakios) ------------------- */

#ifdef XNU_KERNEL_PRIVATE
#include <kern/clock.h>
#include <kern/locks.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>

static LCK_GRP_DECLARE(gnss_lck_grp, "hakios-gnss");
static LCK_MTX_DECLARE(gnss_lock, &gnss_lck_grp);
static uint32_t gnss_state;
static struct hakios_gnss_fix gnss_fix = { .speed_mms = -1, .heading_mdeg = -1 };
static struct hakios_gnss_sky gnss_sky;

void
hakios_gnss_publish(uint32_t state, const struct hakios_gnss_fix *fix, const struct hakios_gnss_sky *sky)
{
	lck_mtx_lock(&gnss_lock);
	gnss_state = state;
	if (fix != NULL) {
		gnss_fix = *fix;
	}
	if (sky != NULL) {
		gnss_sky = *sky;
	}
	lck_mtx_unlock(&gnss_lock);
}

static long
decimal(const char *text, const char **end)
{
	bool negative = *text == '-';
	long value = 0;

	if (negative) {
		text++;
	}
	while (*text >= '0' && *text <= '9' && value < 10000000000L) {
		value = value * 10 + (*text++ - '0');
	}
	*end = text;
	return negative ? -value : value;
}

/*
 * Boot-arg gnss_fake=LAT_E7,LON_E7 (degrees * 10^7, e.g. gnss_fake=-337000000,1512000000): a fix there, every second,
 * marked HAKIOS_GNSS_FAKE, for trying the way to locationd on a phone whose modem does not run. A real fix wins.
 */
static void
gnss_fake_now(void)
{
	static int parsed;              /* 0 not yet, 1 there is one, -1 none */
	static int32_t lat, lon;
	clock_sec_t seconds;
	clock_usec_t microseconds;

	if (parsed == 0) {
		char text[48] = "";
		const char *end = NULL;
		long a, b;

		parsed = -1;
		if (PE_parse_boot_arg_str("gnss_fake", text, sizeof(text)) && text[0] != 0) {
			a = decimal(text, &end);
			if (*end == ',') {
				b = decimal(end + 1, &end);
				if (*end == 0 && a >= -900000000 && a <= 900000000 && b >= -1800000000 && b <= 1800000000) {
					lat = (int32_t)a;
					lon = (int32_t)b;
					parsed = 1;
					printf("gnss: gnss_fake: a made-up fix at %ld, %ld (degrees * 10^7) when there is no real one\n", a, b);
				}
			}
		}
	}
	if (parsed != 1 || gnss_state == HAKIOS_GNSS_FIX) {
		return;
	}
	clock_get_calendar_microtime(&seconds, &microseconds);
	if (gnss_fix.utc_ms / 1000 == (uint64_t)seconds && (gnss_fix.flags & HAKIOS_GNSS_FAKE)) {
		return;
	}
	gnss_fix.seq++;
	gnss_fix.flags = HAKIOS_GNSS_FAKE | HAKIOS_GNSS_HAS_POSITION | HAKIOS_GNSS_HAS_ALTITUDE | HAKIOS_GNSS_FINAL;
	gnss_fix.status = 0;
	gnss_fix.lat_e7 = lat;
	gnss_fix.lon_e7 = lon;
	gnss_fix.alt_mm = 10000;
	gnss_fix.hacc_mm = 5000;
	gnss_fix.vacc_mm = 8000;
	gnss_fix.speed_mms = 0;
	gnss_fix.heading_mdeg = -1;
	gnss_fix.utc_ms = (uint64_t)seconds * 1000 + microseconds / 1000;
	gnss_fix.sats_used = 0;
}

static int
sysctl_gnss_state SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value;

	lck_mtx_lock(&gnss_lock);
	gnss_fake_now();
	value = (int)gnss_state;
	if (value < HAKIOS_GNSS_FIX && (gnss_fix.flags & HAKIOS_GNSS_FAKE)) {
		value = HAKIOS_GNSS_FIX;
	}
	lck_mtx_unlock(&gnss_lock);
	return SYSCTL_OUT(req, &value, sizeof(value));
}

/* "seq flags status lat_e7 lon_e7 alt_mm hacc_mm vacc_mm speed_mms heading_mdeg utc_ms sats_used" */
static int
sysctl_gnss_fix SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[160];
	int length;

	lck_mtx_lock(&gnss_lock);
	gnss_fake_now();
	length = snprintf(text, sizeof(text), "%u %u %u %d %d %d %u %u %d %d %llu %u", gnss_fix.seq, gnss_fix.flags,
	    gnss_fix.status, gnss_fix.lat_e7, gnss_fix.lon_e7, gnss_fix.alt_mm, gnss_fix.hacc_mm, gnss_fix.vacc_mm,
	    gnss_fix.speed_mms, gnss_fix.heading_mdeg, (unsigned long long)gnss_fix.utc_ms, gnss_fix.sats_used);
	lck_mtx_unlock(&gnss_lock);
	return SYSCTL_OUT(req, text, (size_t)length + 1);
}

/* "seq seen tracked best_snr_db engine" */
static int
sysctl_gnss_sky SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[80];
	int length;

	lck_mtx_lock(&gnss_lock);
	length = snprintf(text, sizeof(text), "%u %u %u %u %u", gnss_sky.seq, gnss_sky.seen, gnss_sky.tracked,
	    gnss_sky.best_snr_db, gnss_sky.engine);
	lck_mtx_unlock(&gnss_lock);
	return SYSCTL_OUT(req, text, (size_t)length + 1);
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, gnss_state, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_gnss_state, "I",
    "GNSS: 0 no location service, 1 service found, 2 events registered, 3 session started, 4 fix");
SYSCTL_PROC(_kern_hakios, OID_AUTO, gnss_fix, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_gnss_fix, "A",
    "the last fix: seq flags status lat_e7 lon_e7 alt_mm hacc_mm vacc_mm speed_mms heading_mdeg utc_ms sats_used");
SYSCTL_PROC(_kern_hakios, OID_AUTO, gnss_sky, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_gnss_sky, "A",
    "the satellites: seq seen tracked best_snr_db engine(1 on, 2 off)");
#else
void
hakios_gnss_publish(uint32_t state, const struct hakios_gnss_fix *fix, const struct hakios_gnss_sky *sky)
{
	(void)state;
	(void)fix;
	(void)sky;
}
#endif
