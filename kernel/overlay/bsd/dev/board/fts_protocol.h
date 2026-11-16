/* FT3658U application protocol, independent of the Qualcomm transport.
 * Layout/CRC verified against OKX focaltech_3658u at 08bbb0c150ea.
 * The read begins at register 0x01; the vendor buffer's command byte is absent.
 */
#ifndef RENOIR_FTS_PROTOCOL_H
#define RENOIR_FTS_PROTOCOL_H

#define FTS_CONTACTS 10
#define FTS_REPORT_BYTES (2 + 6 * FTS_CONTACTS)
#define FTS_READ_PREFIX 7
#define FTS_MAX_DATA FTS_REPORT_BYTES
#define FTS_SPI_BYTES (FTS_READ_PREFIX + FTS_MAX_DATA + 2)
#define FTS_MAX_X 1080
#define FTS_MAX_Y 2400

struct fts_frame {
	uint16_t active;
	uint16_t x[FTS_CONTACTS], y[FTS_CONTACTS];
};

static bool
fts_report_empty(const uint8_t *data, uint32_t length)
{
	if (length != FTS_REPORT_BYTES) { return false; }
	for (uint32_t i = 0; i < length; i++) {
		if (data[i] != 0xff) { return false; }
	}
	return true;
}

/* Reflected 0x1021 polynomial, seed 0xffff, no final xor; wire CRC is LE. */
static uint16_t
fts_crc(const uint8_t *data, uint32_t length)
{
	uint16_t crc = 0xffff;
	for (uint32_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
		}
	}
	return crc;
}

/* Decode completely before publishing any events. Malformed frames change no contacts. */
static bool
fts_decode(const uint8_t *data, uint32_t length, struct fts_frame *frame)
{
	struct fts_frame result = { 0 };
	uint16_t ids = 0;
	unsigned active = 0, count;
	if (length != FTS_REPORT_BYTES || (count = data[1] & 0xf) > FTS_CONTACTS) {
		return false;
	}
	for (unsigned i = 0; i < FTS_CONTACTS; i++) {
		const uint8_t *point = data + 2 + 6 * i;
		unsigned id = point[2] >> 4, event = point[0] >> 6;
		if (id >= FTS_CONTACTS) {
			break; /* vendor sentinel (unused slots are 0xff) */
		}
		if ((ids & (1u << id)) || event == 3) {
			return false;
		}
		ids |= 1u << id;
		if (event == 1) { /* explicit UP; it need not carry a valid coordinate */
			continue;
		}
		result.x[id] = (point[0] & 0xf) << 8 | point[1];
		result.y[id] = (point[2] & 0xf) << 8 | point[3];
		if (result.x[id] > FTS_MAX_X || result.y[id] > FTS_MAX_Y) {
			return false;
		}
		result.active |= 1u << id;
		active++;
	}
	if (active != count) {
		return false;
	}
	*frame = result;
	return true;
}
#endif
