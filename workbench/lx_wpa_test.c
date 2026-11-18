/*
 * Host tests of the WPA2 handshake of the Wi-Fi driver's object (kernel/lx/src/lx_wpa.c), compiled as it is: the file is
 * included whole, outside the kernel, where it needs nothing but <stdint.h> and <string.h>. The station's side is the
 * file's; the access point's is frames made beforehand with Python's hashlib and "cryptography" (below, with how they were
 * made), so that this needs nothing but a C compiler.
 *
 *   lx_wpa_test                 the scenarios' names
 *   lx_wpa_test SCENARIO        one scenario a process
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/lx/src/lx_wpa.c"

static int checks, failures;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/*
 * The access point's frames and what the station is to answer, made with Python 3 thus:
 *   PMK = hashlib.pbkdf2_hmac("sha1", b"password", b"ssid", 4096, 32); the access point 02:00:00:00:00:01, the station
 *   02:00:00:00:00:02; the access point's nonce 32 bytes of 0xa1 (a forger's: of 0xee), the station's of 0x5e; the PTK the
 *   standard's PRF of them ("Pairwise key expansion", 48 bytes: KCK, KEK, TK); key data wrapped with the KEK by
 *   cryptography's aes_key_wrap, padded with 0xdd 0; every signed frame's MIC HMAC-SHA1 with the KCK, cut to 16 bytes.
 *   m1, m1x   message 1 (key info 0x008a, counter 1), the access point's nonce and a forger's
 *   m3, m3r   message 3 (0x13ca, counter 2, and again with 3): the RSN element of rsn, the group key gtk (number 1)
 *   m3_other_rsn  message 3 (counter 2) whose RSN element offers TKIP to everybody, not what rsn (the beacons') says
 *   g1, g1r   a group message (0x1382, counter 4, RSC 5), the group key gtk2 (number 2); and again (counter 5, RSC 0)
 *   m2, m2x, m4, m4r, g2, g2r   what the station answers: message 2 to m1 (and to m1x: signed with that one's KCK),
 *             message 4 to m3 and m3r, its answers to g1 and g1r -- key length 0, as answer() leaves it
 *   ieee_pmk  the PMK of "password" and "IEEE" (the example of IEEE 802.11's annex J, the passphrase's key)
 *   rfc3394_wrapped  RFC 3394, 4.1: the key 00112233...eeff wrapped with the KEK 00010203...0e0f
 */
static const struct {
	const char *name, *hex;
} given[] = {
	{ "pmk",
	    "44116ea881531996d8a23af58b376d70f196057429c258f529577a26e727ec1b" },
	{ "m1",
	    "0203005f02008a00100000000000000001a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"
	    "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1000000000000000000000000000000"
	    "0000000000000000000000000000000000000000000000000000000000000000"
	    "000000" },
	{ "m1x",
	    "0203005f02008a00100000000000000001eeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	    "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee000000000000000000000000000000"
	    "0000000000000000000000000000000000000000000000000000000000000000"
	    "000000" },
	{ "m3",
	    "020300970213ca00100000000000000002a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"
	    "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1000000000000000000000000000000"
	    "00000000000000000000000000000000007a9eb37f8036bf67d1f29d2a84d587"
	    "730038f7d5842dde460f864fd27e318af18f94748f972c90a2857aa2f1640c99"
	    "390d50166e18d22acc1a7b36038381e7de20db2900e844d27289d3" },
	{ "m3r",
	    "020300970213ca00100000000000000003a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"
	    "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1000000000000000000000000000000"
	    "0000000000000000000000000000000000ddf3323b339d4a37e4aabe5cdb10b8"
	    "df0038f7d5842dde460f864fd27e318af18f94748f972c90a2857aa2f1640c99"
	    "390d50166e18d22acc1a7b36038381e7de20db2900e844d27289d3" },
	{ "m3_other_rsn",
	    "020300970213ca00100000000000000002a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"
	    "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1000000000000000000000000000000"
	    "00000000000000000000000000000000002049d4f8c49c0c168fe8befc4ef417"
	    "dc003895ae9d73628146bc65d315553cc048936abc56c7665eda75f04f654006"
	    "35fc25669c0b8b3430750873df23c4b64a3d995b1fe1274685d9ea" },
	{ "g1",
	    "0203007f02138200100000000000000004000000000000000000000000000000"
	    "0000000000000000000000000000000000000000000000000000000000000000"
	    "0005000000000000000000000000000000003fea6a6df2535396e43259c50b4a"
	    "d800203cfbc642d383c2fa84be26c2fccf3734ae5c98145bc285b7b35236e78d"
	    "6949ba" },
	{ "g1r",
	    "0203007f02138200100000000000000005000000000000000000000000000000"
	    "0000000000000000000000000000000000000000000000000000000000000000"
	    "00000000000000000000000000000000003be75e9ba245cafaa753d64edb162d"
	    "7f00203cfbc642d383c2fa84be26c2fccf3734ae5c98145bc285b7b35236e78d"
	    "6949ba" },
	{ "m2",
	    "0203007502010a000000000000000000015e5e5e5e5e5e5e5e5e5e5e5e5e5e5e"
	    "5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e000000000000000000000000000000"
	    "0000000000000000000000000000000000fa470fe070da1179149e46b1e11604"
	    "9d001630140100000fac040100000fac040100000fac020000" },
	{ "m2x",
	    "0203007502010a000000000000000000015e5e5e5e5e5e5e5e5e5e5e5e5e5e5e"
	    "5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e000000000000000000000000000000"
	    "00000000000000000000000000000000000675a30fd724cee3c8371d919b4a4d"
	    "1a001630140100000fac040100000fac040100000fac020000" },
	{ "m4",
	    "0203005f02030a00000000000000000002000000000000000000000000000000"
	    "0000000000000000000000000000000000000000000000000000000000000000"
	    "0000000000000000000000000000000000262347b7f43dd8aebf0a4451411f73"
	    "ab0000" },
	{ "m4r",
	    "0203005f02030a00000000000000000003000000000000000000000000000000"
	    "0000000000000000000000000000000000000000000000000000000000000000"
	    "0000000000000000000000000000000000a2e8d7cc0d0726446ab38162c3bc57"
	    "930000" },
	{ "g2",
	    "0203005f02030200000000000000000004000000000000000000000000000000"
	    "0000000000000000000000000000000000000000000000000000000000000000"
	    "00000000000000000000000000000000007a77f5aca0e6376d59fee468e84ed5"
	    "f80000" },
	{ "g2r",
	    "0203005f02030200000000000000000005000000000000000000000000000000"
	    "0000000000000000000000000000000000000000000000000000000000000000"
	    "00000000000000000000000000000000004053ffe428c0963d30149dbe680f0a"
	    "a10000" },
	{ "tk",
	    "2eb93012f3123881bc4ec06e02797bdb" },
	{ "kck",
	    "6aa3775ee9fceedbeb90eff35f5cd1fb" },
	{ "gtk",
	    "000102030405060708090a0b0c0d0e0f" },
	{ "gtk2",
	    "101112131415161718191a1b1c1d1e1f" },
	{ "rsn",
	    "30140100000fac040100000fac040100000fac020000" },
	{ "ieee_pmk",
	    "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e" },
	{ "rfc3394_wrapped",
	    "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5" },
};

/* ---- the vectors, as bytes ---------------------------------------------------------------------------------- */

#define GIVEN	(sizeof(given) / sizeof(given[0]))

static uint8_t given_bytes[GIVEN][256];
static unsigned given_length[GIVEN];

static unsigned
hex_digit(char c)
{
	return c >= 'a' ? (unsigned)(c - 'a' + 10) : (unsigned)(c - '0');
}

static void
unhex(void)
{
	for (unsigned i = 0; i < GIVEN; i++) {
		given_length[i] = (unsigned)strlen(given[i].hex) / 2;
		for (unsigned j = 0; j < given_length[i]; j++) {
			given_bytes[i][j] = (uint8_t)(hex_digit(given[i].hex[2 * j]) << 4 | hex_digit(given[i].hex[2 * j + 1]));
		}
	}
}

/* The vector of that name, and its length. */
static const uint8_t *
vector(const char *name, unsigned *length)
{
	for (unsigned i = 0; i < GIVEN; i++) {
		if (strcmp(given[i].name, name) == 0) {
			if (length != NULL) {
				*length = given_length[i];
			}
			return given_bytes[i];
		}
	}
	printf("no vector %s\n", name);
	exit(2);
}

#define V(name) vector(name, NULL)

static const uint8_t own[6] = { 0x02, 0, 0, 0, 0, 0x02 }, peer[6] = { 0x02, 0, 0, 0, 0, 0x01 };

/* The station as lx_80211.c sets it up for a join: the key, both addresses, its nonce, its RSN element and the beacons'. */
static void
station(struct lx_wpa *wpa)
{
	memset(wpa, 0, sizeof(*wpa));
	lx_wpa_pmk((const unsigned char *)"password", 8, (const unsigned char *)"ssid", 4, wpa->pmk);
	memcpy(wpa->own, own, 6);
	memcpy(wpa->peer, peer, 6);
	memset(wpa->own_nonce, 0x5e, 32);
	wpa->rsn = vector("rsn", &wpa->rsn_length);
	wpa->target_rsn = vector("rsn", &wpa->target_rsn_length);
	wpa->group_cipher = 4;
}

static uint8_t reply[LX_WPA_REPLY_MOST];
static unsigned reply_length;
static const char *why;

/* The access point's frame of that name to the station: what it calls for, and the station's reply in `reply`. */
static int
message(struct lx_wpa *wpa, const char *name)
{
	unsigned length;
	const uint8_t *frame = vector(name, &length);

	memset(reply, 0, sizeof(reply));
	return lx_wpa_message(wpa, frame, length, reply, &reply_length, &why);
}

/* The station's reply is the frame of that name. */
static int
replied(const char *name)
{
	unsigned length;
	const uint8_t *frame = vector(name, &length);

	return reply_length == length && memcmp(reply, frame, length) == 0;
}

static int
all_zero(const uint8_t *p, unsigned length)
{
	unsigned or = 0;

	for (unsigned i = 0; i < length; i++) {
		or |= p[i];
	}
	return or == 0;
}

/* ---- scenarios -------------------------------------------------------------------------------------------- */

/* The arithmetic against the standards' own examples: a passphrase's key, and an unwrapped key. */
static void
scenario_standards(void)
{
	uint8_t key[32], plain[16], wrong[24];
	static const uint8_t kek[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
	static const uint8_t unwrapped[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
		0xff };
	unsigned length;
	const uint8_t *wrapped = vector("rfc3394_wrapped", &length);

	lx_wpa_pmk((const unsigned char *)"password", 8, (const unsigned char *)"IEEE", 4, key);
	CHECK(memcmp(key, V("ieee_pmk"), 32) == 0, "the PMK of \"password\" and \"IEEE\" is the standard's");
	lx_wpa_pmk((const unsigned char *)"password", 8, (const unsigned char *)"ssid", 4, key);
	CHECK(memcmp(key, V("pmk"), 32) == 0, "the PMK of \"password\" and \"ssid\" is hashlib's");
	CHECK(length == sizeof(wrong) && aes_unwrap(kek, wrapped, length, plain) == 0 && memcmp(plain, unwrapped, 16) == 0,
	    "RFC 3394's key unwraps");
	memcpy(wrong, wrapped, sizeof(wrong));
	wrong[sizeof(wrong) - 1] ^= 1;
	CHECK(aes_unwrap(kek, wrong, sizeof(wrong), plain) != 0, "a wrapped key changed in one bit does not");
}

/* Messages 1 and 3, answered with 2 and 4 signed as the access point checks them; the keys are the access point's. */
static void
scenario_good_handshake(void)
{
	struct lx_wpa wpa;
	int said;

	station(&wpa);
	said = message(&wpa, "m1");
	CHECK(said == LX_WPA_ANSWER && replied("m2"), "message 1 is answered with message 2 (%d, %u bytes: %s)", said, reply_length,
	    why);
	said = message(&wpa, "m3");
	CHECK(said == LX_WPA_KEYS && replied("m4"), "message 3 is answered with message 4 (%d, %u bytes: %s)", said, reply_length,
	    why);
	CHECK(memcmp(wpa.pairwise, V("tk"), 16) == 0, "the pairwise key is the access point's");
	CHECK(memcmp(wpa.kck, V("kck"), 16) == 0, "so is the key the messages are signed with");
	CHECK(wpa.group_index == 1 && wpa.group_length == 16 && memcmp(wpa.group, V("gtk"), 16) == 0 && all_zero(wpa.group + 16, 16),
	    "the group key is message 3's (number %u, %u bytes)", wpa.group_index, wpa.group_length);
	said = message(&wpa, "g1");
	CHECK(said == LX_WPA_GROUP && replied("g2"), "a new group key is answered and is to be used (%d: %s)", said, why);
	CHECK(wpa.group_index == 2 && memcmp(wpa.group, V("gtk2"), 16) == 0 && wpa.group_counter[0] == 5,
	    "the new group key, its number and its packet number are the group message's (number %u, counter %u)",
	    wpa.group_index, wpa.group_counter[0]);
}

/*
 * Somebody in range sends an unsigned message 1 of a nonce of its own between the access point's message 1 and its
 * message 3: the station answers it, as it cannot tell the two apart, but the access point's message 3 is still taken
 * and the keys are those of the access point's nonce. Once they are, a message 1 changes nothing.
 */
static void
scenario_forged_message_1(void)
{
	struct lx_wpa wpa;
	int said;

	station(&wpa);
	said = message(&wpa, "m1");
	CHECK(said == LX_WPA_ANSWER && replied("m2"), "message 1 is answered (%d: %s)", said, why);
	said = message(&wpa, "m1x");
	CHECK(said == LX_WPA_ANSWER && replied("m2x"), "the forged one is answered with its own keys (%d: %s)", said, why);
	said = message(&wpa, "m3");
	CHECK(said == LX_WPA_KEYS && replied("m4"), "the access point's message 3 is taken (%d: %s)", said, why);
	CHECK(memcmp(wpa.pairwise, V("tk"), 16) == 0, "the pairwise key is the access point's");
	said = message(&wpa, "m1x");
	CHECK(said == LX_WPA_NOTHING && reply_length == 0, "a message 1 after that is not answered (%d: %s)", said, why);
	CHECK(memcmp(wpa.pairwise, V("tk"), 16) == 0 && memcmp(wpa.kck, V("kck"), 16) == 0, "and changes no key");
	said = message(&wpa, "g1");
	CHECK(said == LX_WPA_GROUP && replied("g2"), "a group message signed with the keys is taken (%d: %s)", said, why);
}

/*
 * The access point sends a message again because our answer did not reach it (or somebody plays it again): it is
 * answered, but the key it brings is the one there is -- not one to install again, which would let the driver start the
 * key's packet numbers over -- and the packet number the group's frames have got to stays.
 */
static void
scenario_group_sent_again(void)
{
	struct lx_wpa wpa;
	int said;

	station(&wpa);
	message(&wpa, "m1");
	said = message(&wpa, "m3");
	CHECK(said == LX_WPA_KEYS, "message 3 is taken (%d: %s)", said, why);
	said = message(&wpa, "m3r");
	CHECK(said == LX_WPA_AGAIN && replied("m4r"), "message 3 again is answered, and nothing is to be installed (%d: %s)", said,
	    why);
	CHECK(memcmp(wpa.pairwise, V("tk"), 16) == 0, "the pairwise key is as it was");
	said = message(&wpa, "g1");
	CHECK(said == LX_WPA_GROUP && wpa.group_counter[0] == 5, "a new group key is to be used (%d: %s; counter %u)", said, why,
	    wpa.group_counter[0]);
	said = message(&wpa, "g1r");
	CHECK(said == LX_WPA_AGAIN && replied("g2r"), "the same group key again is answered, and not to be installed (%d: %s)",
	    said, why);
	CHECK(wpa.group_index == 2 && memcmp(wpa.group, V("gtk2"), 16) == 0, "the group key is as it was (number %u)", wpa.group_index);
	CHECK(wpa.group_counter[0] == 5, "its packet number is not taken back to the repeated message's (%u)", wpa.group_counter[0]);
	said = message(&wpa, "g1r");
	CHECK(said == LX_WPA_BAD && reply_length == 0, "the repeat played once more is refused by its counter (%d: %s)", said, why);
}

/* Message 3, signed with the right keys, says the access point offers what its beacons did not: the network is left. */
static void
scenario_rsn_mismatch(void)
{
	struct lx_wpa wpa;
	int said;

	station(&wpa);
	message(&wpa, "m1");
	said = message(&wpa, "m3_other_rsn");
	CHECK(said == LX_WPA_REFUSED && reply_length == 0, "message 3 is refused and not answered (%d, %u bytes: %s)", said,
	    reply_length, why);
	CHECK(!wpa.have_keys && !wpa.replay_known && all_zero(wpa.pairwise, 16) && wpa.group_length == 0,
	    "nothing of it is taken");
}

/* Once message 3 has proven the keys the passphrase's key is not needed again, and is not kept. */
static void
scenario_pmk_wiped(void)
{
	struct lx_wpa wpa;

	station(&wpa);
	message(&wpa, "m1");
	CHECK(memcmp(wpa.pmk, V("pmk"), 32) == 0, "the PMK is there for message 3");
	CHECK(message(&wpa, "m3") == LX_WPA_KEYS, "message 3 is taken (%s)", why);
	CHECK(all_zero(wpa.pmk, 32), "the PMK is wiped");
	CHECK(message(&wpa, "m3r") != LX_WPA_BAD && message(&wpa, "g1") == LX_WPA_GROUP, "what comes after does without it (%s)",
	    why);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "standards", scenario_standards },
		{ "good_handshake", scenario_good_handshake },
		{ "forged_message_1", scenario_forged_message_1 },
		{ "group_sent_again", scenario_group_sent_again },
		{ "rsn_mismatch", scenario_rsn_mismatch },
		{ "pmk_wiped", scenario_pmk_wiped },
	};

	unhex();
	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			printf("lx_wpa_test %s: %d checks, %d failed\n", argv[1], checks, failures);
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
