/* Part of the loader (included by loader.c, not compiled alone): this unit's identity, from one seed. */
/*
 * This unit's identity: what the factory gives each phone and iBoot reads out of it -- the serial number,
 * the logic board's serial number, the chip's unique ID (ECID), the Wi-Fi and Bluetooth addresses, the
 * hash of the boot manifest. MobileGestalt builds the device's identifier (UDID) from them, and waits for
 * ever where one is missing.
 *
 * All of them come from one 64-bit seed, so that a phone keeps its identity from one boot to the next when
 * it is given the same seed (boot argument identity=<16 hex digits>; tools/identity.py makes one and keeps
 * it), and two phones given different seeds never share one. Without a seed the loader takes a random one,
 * and the phone is another device at every boot.
 *
 * Each value has the form a real one has:
 *   serial number   10 characters, digits and capital letters without I and O (the randomised serial
 *                   numbers Apple has given its devices since 2021)
 *   MLB serial      17 characters of the same alphabet
 *   ECID            a 52-bit number, never 0
 *   addresses       6 bytes, unicast and locally administered (bit 1 of the first byte set, bit 0 clear):
 *                   no maker's prefix is claimed
 *   manifest hash   48 bytes, the length of a SHA-384
 */
#define UNIT_SERIAL_LENGTH      10
#define UNIT_MLB_LENGTH         17

struct unit_identity {
	uint64_t seed;
	char serial[UNIT_SERIAL_LENGTH + 1];
	char mlb[UNIT_MLB_LENGTH + 1];
	uint64_t ecid;
	uint8_t wifi_address[6], bluetooth_address[6];
	uint8_t manifest_hash[48];
};

/* SplitMix64: a 64-bit seed spread over as many well-mixed 64-bit values as are drawn */
static uint64_t
unit_next(uint64_t *state)
{
	uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static void
unit_characters(uint64_t *state, char *out, unsigned count)
{
	static const char alphabet[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";       /* 34: no I, no O */

	for (unsigned i = 0; i < count; i++) {
		out[i] = alphabet[unit_next(state) % (sizeof(alphabet) - 1)];
	}
	out[count] = '\0';
}

static void
unit_address(uint64_t *state, uint8_t address[6])
{
	uint64_t r = unit_next(state);

	for (unsigned i = 0; i < 6; i++) {
		address[i] = (uint8_t)(r >> (8 * i));
	}
	address[0] = (uint8_t)((address[0] & ~3u) | 2u);       /* unicast, locally administered */
}

static void
unit_from_seed(uint64_t seed, struct unit_identity *id)
{
	uint64_t state = seed;

	memset(id, 0, sizeof(*id));
	id->seed = seed;
	unit_characters(&state, id->serial, UNIT_SERIAL_LENGTH);
	unit_characters(&state, id->mlb, UNIT_MLB_LENGTH);
	id->ecid = (unit_next(&state) & 0x000fffffffffffffULL) | 0x0001000000000000ULL;
	unit_address(&state, id->wifi_address);
	unit_address(&state, id->bluetooth_address);
	if (id->bluetooth_address[5] == id->wifi_address[5]) {
		id->bluetooth_address[5] ^= 1;          /* (never the Wi-Fi's) */
	}
	for (unsigned i = 0; i < sizeof(id->manifest_hash); i += 8) {
		uint64_t r = unit_next(&state);

		for (unsigned j = 0; j < 8; j++) {
			id->manifest_hash[i + j] = (uint8_t)(r >> (8 * j));
		}
	}
}

/* identity=<16 hex digits> among the boot arguments: the seed it gives, 1; none or not 16 hex digits, 0 */
static int
unit_seed_from_args(const char *args, uint64_t *seed)
{
	static const char key[] = "identity=";

	for (const char *c = args; *c; c++) {
		uint64_t value = 0;
		unsigned digits = 0, k = 0;

		while (key[k] != '\0' && c[k] == key[k]) {
			k++;
		}
		if ((c != args && c[-1] != ' ') || key[k] != '\0') {
			continue;
		}
		for (c += sizeof(key) - 1; *c && *c != ' '; c++, digits++) {
			unsigned d = *c >= '0' && *c <= '9' ? (unsigned)(*c - '0') : *c >= 'a' && *c <= 'f' ? (unsigned)(*c - 'a' + 10) :
			    *c >= 'A' && *c <= 'F' ? (unsigned)(*c - 'A' + 10) : 16u;

			if (d == 16) {
				return 0;
			}
			value = value << 4 | d;
		}
		if (digits != 16) {
			return 0;
		}
		*seed = value;
		return 1;
	}
	return 0;
}
