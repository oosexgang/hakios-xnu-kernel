/*
 * The arithmetic of WPA2 with a passphrase (lx_wpa.h): SHA-1 and HMAC over it (FIPS 180, RFC
 * 2104), the key of a name and a passphrase (PBKDF2, RFC 8018), the function that makes the
 * handshake's keys out of that key, two addresses and two nonces (IEEE 802.11-2020, 12.7.1.2),
 * AES decryption and the unwrapping of a key with it (FIPS 197, RFC 3394), and the messages of
 * the four-way and the group-key handshake (12.7.6, 12.7.7; figure 12-35 is the layout).
 *
 * Nothing here is taken from another implementation; workbench/lx_wpa_test.c checks it
 * against the standards' own examples and against an access point played with Python's
 * hashlib and "cryptography". Nothing here is written for speed, and nothing here says a key
 * in a message.
 */
#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#else
#include <stdint.h>
#include <string.h>
#endif

#include "lx_wpa.h"

/*
 * Zeros over what held a key or a step of making one, through a volatile pointer: a memset() of a
 * buffer that is not read again before it goes out of scope is a store the compiler may leave out.
 */
static void
wipe(void *buffer, unsigned length)
{
	volatile uint8_t *at = buffer;

	while (length-- != 0) {
		*at++ = 0;
	}
}

/* ---- SHA-1 ------------------------------------------------------------------------------------ */

struct sha1 {
	uint32_t h[5];
	uint64_t length;			/* bytes so far */
	uint8_t block[64];
};

static uint32_t
rol(uint32_t x, unsigned n)
{
	return (x << n | x >> (32 - n));
}

static void
sha1_block(uint32_t h[5], const uint8_t *p)
{
	uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

	for (unsigned t = 0; t < 16; t++) {
		w[t] = (uint32_t)p[4 * t] << 24 | (uint32_t)p[4 * t + 1] << 16 | (uint32_t)p[4 * t + 2] << 8 | p[4 * t + 3];
	}
	for (unsigned t = 16; t < 80; t++) {
		w[t] = rol(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
	}
	for (unsigned t = 0; t < 80; t++) {
		uint32_t f = t < 20 ? ((b & c) | (~b & d)) + 0x5a827999 : t < 40 ? (b ^ c ^ d) + 0x6ed9eba1 : t < 60 ?
		    ((b & c) | (b & d) | (c & d)) + 0x8f1bbcdc : (b ^ c ^ d) + 0xca62c1d6;
		uint32_t next = rol(a, 5) + f + e + w[t];

		e = d;
		d = c;
		c = rol(b, 30);
		b = a;
		a = next;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	wipe(w, sizeof(w));			/* (the schedule is the block, the key's own bytes in HMAC's first one) */
}

static void
sha1_begin(struct sha1 *s)
{
	s->h[0] = 0x67452301;
	s->h[1] = 0xefcdab89;
	s->h[2] = 0x98badcfe;
	s->h[3] = 0x10325476;
	s->h[4] = 0xc3d2e1f0;
	s->length = 0;
}

static void
sha1_more(struct sha1 *s, const uint8_t *data, unsigned length)
{
	for (unsigned i = 0; i < length; i++) {
		s->block[s->length++ % 64] = data[i];
		if (s->length % 64 == 0) {
			sha1_block(s->h, s->block);
		}
	}
}

static void
sha1_end(struct sha1 *s, uint8_t digest[20])
{
	uint64_t bits = s->length * 8;
	uint8_t tail[8];

	sha1_more(s, (const uint8_t *)"\x80", 1);
	while (s->length % 64 != 56) {
		sha1_more(s, (const uint8_t *)"", 1);
	}
	for (unsigned i = 0; i < 8; i++) {
		tail[i] = (uint8_t)(bits >> (56 - 8 * i));
	}
	sha1_more(s, tail, 8);
	for (unsigned i = 0; i < 20; i++) {
		digest[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
	}
}

/* HMAC-SHA1 of several pieces in a row. */
static void
hmac_sha1(const uint8_t *key, unsigned key_length, const uint8_t *const pieces[], const unsigned lengths[], unsigned count,
    uint8_t digest[20])
{
	uint8_t pad[64], hashed[20], inner[20];
	struct sha1 s;

	if (key_length > 64) {
		sha1_begin(&s);
		sha1_more(&s, key, key_length);
		sha1_end(&s, hashed);
		key = hashed;
		key_length = 20;
	}
	for (unsigned i = 0; i < 64; i++) {
		pad[i] = (i < key_length ? key[i] : 0) ^ 0x36;
	}
	sha1_begin(&s);
	sha1_more(&s, pad, 64);
	for (unsigned i = 0; i < count; i++) {
		sha1_more(&s, pieces[i], lengths[i]);
	}
	sha1_end(&s, inner);
	for (unsigned i = 0; i < 64; i++) {
		pad[i] ^= 0x36 ^ 0x5c;
	}
	sha1_begin(&s);
	sha1_more(&s, pad, 64);
	sha1_more(&s, inner, 20);
	sha1_end(&s, digest);
	wipe(pad, sizeof(pad));
	wipe(inner, sizeof(inner));
	wipe(hashed, sizeof(hashed));
	wipe(&s, sizeof(s));			/* (its state and its last block are the key's, padded) */
}

/* ---- the key of a name and a passphrase -------------------------------------------------------- */

/* PBKDF2 with HMAC-SHA1, 4096 rounds, 32 bytes: two blocks of 20, of the second the first 12. */
void
lx_wpa_pmk(const unsigned char *passphrase, unsigned passphrase_length, const unsigned char *name, unsigned name_length,
    unsigned char pmk[32])
{
	for (unsigned block = 1; block <= 2; block++) {
		const uint8_t number[4] = { 0, 0, 0, (uint8_t)block };
		const uint8_t *first[2] = { name, number };
		const unsigned first_lengths[2] = { name_length, 4 };
		uint8_t u[20], sum[20];

		hmac_sha1(passphrase, passphrase_length, first, first_lengths, 2, u);
		memcpy(sum, u, 20);
		for (unsigned round = 1; round < 4096; round++) {
			const uint8_t *again[1] = { u };
			const unsigned again_lengths[1] = { 20 };

			hmac_sha1(passphrase, passphrase_length, again, again_lengths, 1, u);
			for (unsigned i = 0; i < 20; i++) {
				sum[i] ^= u[i];
			}
		}
		memcpy(pmk + 20 * (block - 1), sum, block == 1 ? 20 : 12);
		wipe(u, sizeof(u));
		wipe(sum, sizeof(sum));
	}
}

/* The standard's PRF: HMAC-SHA1 of (label, a zero byte, data, a counter byte), block after block. */
static void
prf(const uint8_t *key, unsigned key_length, const char *label, const uint8_t *data, unsigned data_length, uint8_t *out,
    unsigned out_length)
{
	uint8_t zero = 0, counter = 0, digest[20];
	const uint8_t *pieces[4] = { (const uint8_t *)label, &zero, data, &counter };
	const unsigned lengths[4] = { (unsigned)strlen(label), 1, data_length, 1 };

	for (unsigned done = 0; done < out_length; done += 20, counter++) {
		hmac_sha1(key, key_length, pieces, lengths, 4, digest);
		memcpy(out + done, digest, out_length - done < 20 ? out_length - done : 20);
	}
	wipe(digest, sizeof(digest));
}

/* ---- AES-128, decryption only, and the unwrapping of a key --------------------------------------- */

static uint8_t sbox[256], inverse_sbox[256];

static uint8_t
times(uint8_t a, uint8_t b)			/* in GF(2^8) modulo x^8 + x^4 + x^3 + x + 1 */
{
	uint8_t product = 0;

	for (; b != 0; b >>= 1) {
		if (b & 1) {
			product ^= a;
		}
		a = (uint8_t)(a << 1 ^ (a & 0x80 ? 0x1b : 0));
	}
	return (product);
}

/* FIPS 197, 5.1.1: a byte's inverse in the field, then the affine map. */
static void
aes_tables(void)
{
	if (sbox[0] != 0) {
		return;
	}
	for (unsigned x = 0; x < 256; x++) {
		uint8_t inverse = 0, s;

		for (unsigned y = 1; y < 256 && x != 0; y++) {
			if (times((uint8_t)x, (uint8_t)y) == 1) {
				inverse = (uint8_t)y;
				break;
			}
		}
		s = inverse;
		for (unsigned i = 1; i < 5; i++) {
			s ^= (uint8_t)(inverse << i | inverse >> (8 - i));
		}
		s ^= 0x63;
		sbox[x] = s;
		inverse_sbox[s] = (uint8_t)x;
	}
}

static void
aes_keys(const uint8_t key[16], uint8_t round_keys[176])
{
	uint8_t constant = 1;

	aes_tables();
	memcpy(round_keys, key, 16);
	for (unsigned i = 16; i < 176; i += 4) {
		uint8_t word[4];

		memcpy(word, round_keys + i - 4, 4);
		if (i % 16 == 0) {
			uint8_t first = word[0];

			word[0] = sbox[word[1]] ^ constant;
			word[1] = sbox[word[2]];
			word[2] = sbox[word[3]];
			word[3] = sbox[first];
			constant = times(constant, 2);
		}
		for (unsigned j = 0; j < 4; j++) {
			round_keys[i + j] = round_keys[i - 16 + j] ^ word[j];
		}
	}
}

/* FIPS 197, 5.3: the inverse cipher on one block (bytes in column order: byte r of column c is s[4c + r]). */
static void
aes_decrypt(const uint8_t round_keys[176], uint8_t s[16])
{
	for (unsigned i = 0; i < 16; i++) {
		s[i] ^= round_keys[160 + i];
	}
	for (int round = 9; round >= 0; round--) {
		uint8_t t[16];

		for (unsigned c = 0; c < 4; c++) {		/* the rows shifted back, and the bytes substituted back */
			for (unsigned r = 0; r < 4; r++) {
				t[4 * ((c + r) % 4) + r] = inverse_sbox[s[4 * c + r]];
			}
		}
		for (unsigned i = 0; i < 16; i++) {
			t[i] ^= round_keys[16 * round + i];
		}
		if (round == 0) {
			memcpy(s, t, 16);
			wipe(t, sizeof(t));		/* (the last block is part of the key being unwrapped: not left on the stack) */
			break;
		}
		for (unsigned c = 0; c < 4; c++) {		/* the columns mixed back */
			const uint8_t *a = t + 4 * c;

			s[4 * c + 0] = times(a[0], 14) ^ times(a[1], 11) ^ times(a[2], 13) ^ times(a[3], 9);
			s[4 * c + 1] = times(a[0], 9) ^ times(a[1], 14) ^ times(a[2], 11) ^ times(a[3], 13);
			s[4 * c + 2] = times(a[0], 13) ^ times(a[1], 9) ^ times(a[2], 14) ^ times(a[3], 11);
			s[4 * c + 3] = times(a[0], 11) ^ times(a[1], 13) ^ times(a[2], 9) ^ times(a[3], 14);
		}
	}
}

/*
 * RFC 3394, 2.2.2: `wrapped` is 8 bytes longer than what comes out, which goes to `plain`. 0, or -1
 * if the length is not one a wrapped key has or the check value does not come out.
 */
static int
aes_unwrap(const uint8_t kek[16], const uint8_t *wrapped, unsigned length, uint8_t *plain)
{
	unsigned n = length / 8 - 1;
	uint8_t round_keys[176], a[8], block[16];
	int good = 0;

	if (length % 8 != 0 || length < 24) {
		return (-1);
	}
	aes_keys(kek, round_keys);
	memcpy(a, wrapped, 8);
	memcpy(plain, wrapped + 8, 8 * n);
	for (int j = 5; j >= 0; j--) {
		for (unsigned i = n; i >= 1; i--) {
			uint64_t t = (uint64_t)n * (unsigned)j + i;

			memcpy(block, a, 8);
			for (unsigned k = 0; k < 8; k++) {
				block[7 - k] ^= (uint8_t)(t >> (8 * k));
			}
			memcpy(block + 8, plain + 8 * (i - 1), 8);
			aes_decrypt(round_keys, block);
			memcpy(a, block, 8);
			memcpy(plain + 8 * (i - 1), block + 8, 8);
		}
	}
	for (unsigned i = 0; i < 8; i++) {
		good |= a[i] ^ 0xa6;
	}
	wipe(round_keys, sizeof(round_keys));
	wipe(block, sizeof(block));
	wipe(a, sizeof(a));
	return (good == 0 ? 0 : -1);
}

/* ---- the handshake -------------------------------------------------------------------------------- */

/*
 * A key message, after the four bytes every EAPOL frame begins with (version, type 3, length):
 *   4 the kind of descriptor (2: RSN)     5 what the message is (bits below)     7 key length
 *   9 replay counter (8)     17 nonce (32)     49 IV (16)     65 RSC (8)     73 reserved (8)
 *   81 MIC (16)     97 length of the key data     99 key data
 */
#define AT_KIND		4
#define AT_INFO		5
#define AT_REPLAY	9
#define AT_NONCE	17
#define AT_RSC		65
#define AT_MIC		81
#define AT_DATA_LENGTH	97
#define AT_DATA		99
#define MESSAGE_MOST	640

#define INFO_VERSION	0x0007			/* 2: HMAC-SHA1 for the MIC, the AES key wrap for the key data */
#define INFO_PAIRWISE	0x0008
#define INFO_INSTALL	0x0040
#define INFO_ACK	0x0080
#define INFO_MIC	0x0100
#define INFO_SECURE	0x0200
#define INFO_ENCRYPTED	0x1000

static void
mic_of(const uint8_t kck[16], const uint8_t *message, unsigned length, uint8_t mic[16])
{
	const uint8_t zeros[16] = { 0 };
	const uint8_t *pieces[3] = { message, zeros, message + AT_MIC + 16 };
	const unsigned lengths[3] = { AT_MIC, 16, length - AT_MIC - 16 };
	uint8_t digest[20];

	hmac_sha1(kck, 16, pieces, lengths, 3, digest);
	memcpy(mic, digest, 16);
	wipe(digest, sizeof(digest));
}

/*
 * The keys of the handshake and the pairwise key, from the PMK, both addresses, our nonce and the access point's
 * `peer_nonce`, into `keys`: the KCK (0-15), the KEK (16-31) and the TK (32-47).
 */
static void
derive(const struct lx_wpa *wpa, const uint8_t peer_nonce[32], uint8_t keys[48])
{
	uint8_t data[76];
	int own_first = memcmp(wpa->own, wpa->peer, 6) < 0, own_nonce_first = memcmp(wpa->own_nonce, peer_nonce, 32) < 0;

	memcpy(data, own_first ? wpa->own : wpa->peer, 6);
	memcpy(data + 6, own_first ? wpa->peer : wpa->own, 6);
	memcpy(data + 12, own_nonce_first ? wpa->own_nonce : peer_nonce, 32);
	memcpy(data + 44, own_nonce_first ? peer_nonce : wpa->own_nonce, 32);
	prf(wpa->pmk, 32, "Pairwise key expansion", data, sizeof(data), keys, 48);
}

/* Our message in answer to `message`: the same counter, our bits, our nonce and key data if any, signed with `kck`. */
static void
answer(const uint8_t kck[16], const uint8_t *message, unsigned info, const uint8_t *nonce, const uint8_t *data,
    unsigned data_length, uint8_t *reply, unsigned *reply_length)
{
	unsigned length = AT_DATA + data_length;

	memset(reply, 0, length);
	reply[0] = message[0];
	reply[1] = 3;
	reply[2] = (uint8_t)((length - 4) >> 8);
	reply[3] = (uint8_t)(length - 4);
	reply[AT_KIND] = 2;
	reply[AT_INFO] = (uint8_t)(info >> 8);
	reply[AT_INFO + 1] = (uint8_t)info;
	memcpy(reply + AT_REPLAY, message + AT_REPLAY, 8);
	if (nonce != NULL) {
		memcpy(reply + AT_NONCE, nonce, 32);
	}
	reply[AT_DATA_LENGTH] = (uint8_t)(data_length >> 8);
	reply[AT_DATA_LENGTH + 1] = (uint8_t)data_length;
	if (data_length != 0) {
		memcpy(reply + AT_DATA, data, data_length);
	}
	mic_of(kck, reply, length, reply + AT_MIC);
	*reply_length = length;
}

/* The first element of the key data that `wanted` takes, or NULL; the key data ends where its padding (0xdd 0) begins. */
static const uint8_t *
key_data_element(const struct lx_wpa *wpa, const uint8_t *data, unsigned length, int (*wanted)(const struct lx_wpa *,
    const uint8_t *))
{
	for (unsigned at = 0; at + 2 <= length && at + 2 + data[at + 1] <= length; at += 2 + data[at + 1]) {
		if (data[at] == 0xdd && data[at + 1] == 0) {
			break;
		}
		if (wanted(wpa, data + at)) {
			return (data + at);
		}
	}
	return (NULL);
}

static int
is_rsn(const struct lx_wpa *wpa, const uint8_t *e)
{
	(void)wpa;
	return (e[0] == 48);
}

/*
 * The group key: an element 0xdd of the suite 00-0f-ac, kind 1. A CCMP group key is 16 bytes, a TKIP one 32 (protection()
 * in lx_80211.c joins those two; group_cipher says which), and its number is 1 to 3 (IEEE 802.11-2020, 12.7.2: number 0 is
 * the pairwise key's): a KDE of another length or number is not taken.
 */
static int
is_group_key(const struct lx_wpa *wpa, const uint8_t *e)
{
	const unsigned want = wpa->group_cipher == 2 ? 32 : 16;

	return (e[0] == 0xdd && e[1] >= 6 + 5 && e[2] == 0x00 && e[3] == 0x0f && e[4] == 0xac && e[5] == 1 && e[1] - 6u == want &&
	       (e[6] & 3) != 0);
}

int
lx_wpa_message(struct lx_wpa *wpa, const unsigned char *message, unsigned length, unsigned char *reply, unsigned *reply_length,
    const char **why)
{
	uint8_t mic[16], data[MESSAGE_MOST], made[48], group[32] = { 0 };
	const uint8_t *kck = wpa->kck, *kek = wpa->kek, *rsn, *e;
	unsigned info, body, data_length, difference = 0, changed = 0, group_length, group_index;
	int first, fresh, said = LX_WPA_BAD;
	const char *nothing;

	why = why != NULL ? why : &nothing;
	*why = "";
	*reply_length = 0;
	if (length < 4 || message[1] != 3) {
		return (LX_WPA_NOTHING);
	}
	body = (unsigned)message[2] << 8 | message[3];
	if (length < AT_DATA || 4 + body > length || 4 + body < AT_DATA || 4 + body > MESSAGE_MOST) {
		*why = "its length is not a key message's";
		return (LX_WPA_BAD);
	}
	length = 4 + body;
	info = (unsigned)message[AT_INFO] << 8 | message[AT_INFO + 1];
	data_length = (unsigned)message[AT_DATA_LENGTH] << 8 | message[AT_DATA_LENGTH + 1];
	if (message[AT_KIND] != 2) {
		*why = "it is not an RSN key message (WPA of the first kind?)";
		return (LX_WPA_BAD);
	}
	if ((info & INFO_VERSION) != 2) {
		*why = "its key descriptor is not version 2 (the network wants another kind of key management)";
		return (LX_WPA_BAD);
	}
	if (AT_DATA + data_length > length) {
		*why = "its key data is longer than the message";
		return (LX_WPA_BAD);
	}

	/*
	 * message 1: the access point's nonce, unsigned. Message 1 starts the four-way handshake and nothing else (IEEE 802.11i,
	 * 8.5.3: the supplicant takes it in the states before the pairwise key is installed): once a signed message has been
	 * accepted (replay_known) the keys are proven and in use, and an unsigned frame -- anyone in range can send one -- must not
	 * re-derive and overwrite the KCK, KEK and TK. It is left alone, as every frame that is not for us to answer is.
	 *
	 * Before that, the keys of a message 1 sign our message 2 and are then forgotten: they are not the keys until a message 3
	 * signed with them has passed every check (the standard's temporary PTK, "TPTK"). Message 3 is checked with keys made
	 * from the nonce it carries itself -- the access point's, the same in its messages 1 and 3 -- and from ours, the same in
	 * every message 2 we send. One unsigned message 1 forged between the access point's and its message 3 then neither
	 * replaces keys in use nor has the genuine message 3 refused, as it would if only the last message 1's keys were kept to
	 * check it with.
	 */
	if ((info & (INFO_PAIRWISE | INFO_ACK | INFO_MIC)) == (INFO_PAIRWISE | INFO_ACK)) {
		if (wpa->replay_known) {
			*why = "it is a message 1 after the keys were proven (an unsigned frame does not change them)";
			return (LX_WPA_NOTHING);
		}
		derive(wpa, message + AT_NONCE, made);
		answer(made, message, 2 | INFO_PAIRWISE | INFO_MIC, wpa->own_nonce, wpa->rsn, wpa->rsn_length, reply, reply_length);
		wipe(made, sizeof(made));
		wpa->answered = 1;
		return (LX_WPA_ANSWER);
	}

	/* everything else is signed, by an access point that has shown it has the key */
	if (!(info & INFO_MIC) || !(info & INFO_ACK)) {
		return (LX_WPA_NOTHING);
	}
	first = (info & INFO_PAIRWISE) && !wpa->have_keys;
	if (first && !wpa->answered) {
		*why = "it is signed, and there has been no message 1 to make the keys from";
		return (LX_WPA_BAD);
	}
	if (!first && !wpa->have_keys) {
		*why = "it is a group message, and the four-way handshake has not come through";
		return (LX_WPA_BAD);
	}
	if (first) {
		derive(wpa, message + AT_NONCE, made);
		kck = made;
		kek = made + 16;
	}
	mic_of(kck, message, length, mic);
	for (unsigned i = 0; i < 16; i++) {
		difference |= mic[i] ^ message[AT_MIC + i];
	}
	if (difference != 0) {
		*why = "its signature is not right (a wrong passphrase gives this)";
		goto done;
	}
	if (wpa->replay_known && memcmp(message + AT_REPLAY, wpa->replay, 8) <= 0) {
		*why = "its counter is not beyond the last one's (a message played again)";
		goto done;
	}
	if ((info & INFO_PAIRWISE) && !first && memcmp(message + AT_NONCE, wpa->peer_nonce, 32) != 0) {
		*why = "its nonce is not that of the message 3 the keys were made with";
		goto done;
	}
	if (!(info & INFO_ENCRYPTED) || aes_unwrap(kek, message + AT_DATA, data_length, data) != 0) {
		*why = "its key data does not unwrap";
		goto done;
	}
	/*
	 * Message 3 repeats the access point's RSN element, now signed: one that is not, byte for byte, the element of its
	 * beacons we joined on says those beacons were not its own -- somebody else's, offering less -- and the network is
	 * left (IEEE 802.11-2020, 12.7.6.4).
	 */
	if (info & INFO_PAIRWISE) {
		rsn = key_data_element(wpa, data, data_length - 8, is_rsn);
		if (wpa->target_rsn == NULL || rsn == NULL || 2u + rsn[1] != wpa->target_rsn_length ||
		    memcmp(rsn, wpa->target_rsn, wpa->target_rsn_length) != 0) {
			*why = "the RSN element in it is not the one of the access point's beacons (a beacon not its own?)";
			said = LX_WPA_REFUSED;
			goto done;
		}
	}
	if ((e = key_data_element(wpa, data, data_length - 8, is_group_key)) == NULL) {
		*why = "there is no group key in it";
		goto done;
	}
	/*
	 * A TKIP key is the access point's way round, its Michael key to send with (bytes 16-23) before the one to receive
	 * with: the station receives with the first, and the driver takes the key to receive with at 24 -- the two swapped, as
	 * wpa_supplicant installs a TKIP GTK.
	 */
	group_length = e[1] - 6u;
	group_index = e[6] & 3;
	memcpy(group, e + 8, 16);
	if (group_length == 32) {
		memcpy(group + 16, e + 8 + 24, 8);
		memcpy(group + 24, e + 8 + 16, 8);
	}

	/* every check passed: what the message brings is taken */
	if (first) {
		memcpy(wpa->kck, made, 16);
		memcpy(wpa->kek, made + 16, 16);
		memcpy(wpa->pairwise, made + 32, 16);
		memcpy(wpa->peer_nonce, message + AT_NONCE, 32);
		wpa->have_keys = 1;
		wipe(wpa->pmk, sizeof(wpa->pmk));	/* (no handshake after this one: message 1 is not taken again) */
	}
	/*
	 * The group key the access point sends again -- its message 3 or group message repeated because our answer was lost,
	 * or played again by somebody else -- is answered, and is not a new key: installing it again would let the driver take
	 * its frames' packet numbers from the start once more, and the counter it has got to is kept.
	 */
	for (unsigned i = 0; i < sizeof(group); i++) {
		changed |= group[i] ^ wpa->group[i];
	}
	fresh = changed != 0 || group_length != wpa->group_length || group_index != wpa->group_index;
	if (fresh) {
		memcpy(wpa->group, group, sizeof(group));
		wpa->group_length = group_length;
		wpa->group_index = group_index;
		memcpy(wpa->group_counter, message + AT_RSC, 8);
	}
	memcpy(wpa->replay, message + AT_REPLAY, 8);
	wpa->replay_known = 1;

	/* message 3 of the four, or message 1 of a new group key */
	if (info & INFO_PAIRWISE) {
		answer(wpa->kck, message, 2 | INFO_PAIRWISE | INFO_MIC | INFO_SECURE, NULL, NULL, 0, reply, reply_length);
		said = first ? LX_WPA_KEYS : fresh ? LX_WPA_GROUP : LX_WPA_AGAIN;
	} else {
		answer(wpa->kck, message, 2 | INFO_MIC | INFO_SECURE, NULL, NULL, 0, reply, reply_length);
		said = fresh ? LX_WPA_GROUP : LX_WPA_AGAIN;
	}
done:
	/* (a failed unwrap leaves partial plaintext of the key data in `data`) */
	wipe(data, sizeof(data));
	wipe(made, sizeof(made));
	wipe(group, sizeof(group));
	return (said);
}
