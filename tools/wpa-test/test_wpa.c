/*
 * Tests of wifi/lx/src/lx_wpa.c on the machine the kernel is built on.
 *
 * Two kinds. The pieces against the examples their own standards give (they are quoted here with
 * where they are from). And whole handshakes against an access point written in Python on
 * implementations that share nothing with ours (reference.py: hashlib, hmac, the "cryptography"
 * package's AES key wrap): what a correct station answers is known there to the byte.
 */
#include <stdio.h>
#include <stdlib.h>

#include "lx_wpa.c"

static unsigned checks, failed;

static void
check(int good, const char *what, int n)
{
	checks++;
	if (!good) {
		failed++;
		printf("FAILED: %s (%d)\n", what, n);
	}
}

static int
hex_is(const unsigned char *bytes, unsigned length, const char *hex)
{
	for (unsigned i = 0; i < length; i++) {
		unsigned value;

		if (sscanf(hex + 2 * i, "%2x", &value) != 1 || value != bytes[i]) {
			return 0;
		}
	}
	return hex[2 * length] == 0;
}

struct handshake {
	const unsigned char *passphrase; size_t passphrase_length;
	const unsigned char *name; size_t name_length;
	const unsigned char *ap; size_t ap_length;
	const unsigned char *station; size_t station_length;
	const unsigned char *snonce; size_t snonce_length;
	const unsigned char *rsn; size_t rsn_length;
	const unsigned char *pmk; size_t pmk_length;
	const unsigned char *tk; size_t tk_length;
	const unsigned char *group; size_t group_length;
	const unsigned char *group2; size_t group2_length;
	const unsigned char *m1; size_t m1_length;
	const unsigned char *m2; size_t m2_length;
	const unsigned char *m3; size_t m3_length;
	const unsigned char *m4; size_t m4_length;
	const unsigned char *g1; size_t g1_length;
	const unsigned char *g2; size_t g2_length;
	const unsigned char *m3_wrong; size_t m3_wrong_length;
	unsigned group_index;
};

#include "cases.inc"

static void
pieces(void)
{
	unsigned char digest[20], pmk[32], out[80], block[16], round_keys[176];
	struct sha1 s;

	/* FIPS 180-4, the two examples of SHA-1 */
	sha1_begin(&s);
	sha1_more(&s, (const unsigned char *)"abc", 3);
	sha1_end(&s, digest);
	check(hex_is(digest, 20, "a9993e364706816aba3e25717850c26c9cd0d89d"), "SHA-1 of \"abc\"", 0);
	sha1_begin(&s);
	sha1_more(&s, (const unsigned char *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
	sha1_end(&s, digest);
	check(hex_is(digest, 20, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"), "SHA-1 of the 56-byte example", 0);

	/* RFC 2202, HMAC-SHA1 test cases 1, 2 and 6 (a key longer than a block) */
	{
		unsigned char key[80];
		const unsigned char *one[1];
		unsigned length[1];

		memset(key, 0x0b, 20);
		one[0] = (const unsigned char *)"Hi There";
		length[0] = 8;
		hmac_sha1(key, 20, one, length, 1, digest);
		check(hex_is(digest, 20, "b617318655057264e28bc0b6fb378c8ef146be00"), "HMAC-SHA1, RFC 2202 case 1", 0);
		one[0] = (const unsigned char *)"what do ya want for nothing?";
		length[0] = 28;
		hmac_sha1((const unsigned char *)"Jefe", 4, one, length, 1, digest);
		check(hex_is(digest, 20, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"), "HMAC-SHA1, RFC 2202 case 2", 0);
		memset(key, 0xaa, 80);
		one[0] = (const unsigned char *)"Test Using Larger Than Block-Size Key - Hash Key First";
		length[0] = 54;
		hmac_sha1(key, 80, one, length, 1, digest);
		check(hex_is(digest, 20, "aa4ae5e15272d00e95705637ce8a3b55ed402112"), "HMAC-SHA1, RFC 2202 case 6", 0);
	}

	/* IEEE 802.11-2020, J.4.2: the three examples of a passphrase's key */
	lx_wpa_pmk((const unsigned char *)"password", 8, (const unsigned char *)"IEEE", 4, pmk);
	check(hex_is(pmk, 32, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"), "PMK, J.4.2 example 1", 0);
	lx_wpa_pmk((const unsigned char *)"ThisIsAPassword", 15, (const unsigned char *)"ThisIsASSID", 11, pmk);
	check(hex_is(pmk, 32, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"), "PMK, J.4.2 example 2", 0);
	lx_wpa_pmk((const unsigned char *)"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 32, (const unsigned char *)"ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", 32,
	    pmk);
	check(hex_is(pmk, 32, "becb93866bb8c3832cb777c2f559807c8c59afcb6eae734885001300a981cc62"), "PMK, J.4.2 example 3", 0);

	/* FIPS 197, C.1: the example of AES-128, backwards */
	{
		static const unsigned char key[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
		static const unsigned char cipher[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4,
			                                  0xc5, 0x5a };
		/* RFC 3394, 4.1: 128 bits of key data wrapped with a 128-bit key */
		static const unsigned char wrapped[24] = { 0x1f, 0xa6, 0x8b, 0x0a, 0x81, 0x12, 0xb4, 0x47, 0xae, 0xf3, 0x4b, 0xd8, 0xfb,
			                                   0x5a, 0x7b, 0x82, 0x9d, 0x3e, 0x86, 0x23, 0x71, 0xd2, 0xcf, 0xe5 };
		unsigned char spoiled[24];

		aes_keys(key, round_keys);
		check(hex_is(round_keys + 160, 16, "13111d7fe3944a17f307a78b4d2b30c5"), "AES-128 key schedule, FIPS 197 C.1: the last "
		    "round key", 0);
		memcpy(block, cipher, 16);
		aes_decrypt(round_keys, block);
		check(hex_is(block, 16, "00112233445566778899aabbccddeeff"), "AES-128 decryption, FIPS 197 C.1", 0);
		check(aes_unwrap(key, wrapped, 24, out) == 0 && hex_is(out, 16, "00112233445566778899aabbccddeeff"),
		    "AES key unwrap, RFC 3394 4.1", 0);
		memcpy(spoiled, wrapped, 24);
		spoiled[17] ^= 1;
		check(aes_unwrap(key, spoiled, 24, out) != 0, "AES key unwrap of spoiled data is refused", 0);
		check(aes_unwrap(key, wrapped, 20, out) != 0, "AES key unwrap of a wrong length is refused", 0);
	}

	/* the PRF and a longer unwrap against the independent implementations */
	{
		unsigned char data[70], plain[40];

		for (unsigned i = 0; i < 70; i++) {
			data[i] = (unsigned char)i;
		}
		prf(prf_key, sizeof(prf_key), "A label", data, 70, out, 61);
		check(memcmp(out, prf_out, 61) == 0, "the PRF, 61 bytes, against Python's hmac", 0);
		check(aes_unwrap(wrap_kek, wrap_out, sizeof(wrap_out), plain) == 0 && plain[0] == 100 && plain[39] == 139,
		    "AES key unwrap of 40 bytes against \"cryptography\"", 0);
	}
}

static void
whole(void)
{
	for (unsigned n = 0; n < sizeof(handshakes) / sizeof(handshakes[0]); n++) {
		const struct handshake *h = &handshakes[n];
		unsigned char reply[LX_WPA_REPLY_MOST], spoiled[MESSAGE_MOST];
		unsigned length = 0;
		const char *why = NULL;
		struct lx_wpa wpa;

		memset(&wpa, 0, sizeof(wpa));
		lx_wpa_pmk(h->passphrase, (unsigned)h->passphrase_length, h->name, (unsigned)h->name_length, wpa.pmk);
		check(memcmp(wpa.pmk, h->pmk, 32) == 0, "the PMK against Python's pbkdf2_hmac", (int)n);
		memcpy(wpa.own, h->station, 6);
		memcpy(wpa.peer, h->ap, 6);
		memcpy(wpa.own_nonce, h->snonce, 32);
		wpa.rsn = h->rsn;
		wpa.rsn_length = (unsigned)h->rsn_length;

		/* a signed message before any message 1 */
		check(lx_wpa_message(&wpa, h->m3, (unsigned)h->m3_length, reply, &length, &why) == LX_WPA_BAD, "message 3 first is refused",
		    (int)n);
		check(lx_wpa_message(&wpa, h->m1, (unsigned)h->m1_length, reply, &length, &why) == LX_WPA_ANSWER &&
		    length == h->m2_length && memcmp(reply, h->m2, length) == 0, "message 2 is the reference's", (int)n);
		check(memcmp(wpa.pairwise, h->tk, 16) == 0, "the pairwise key is the reference's", (int)n);

		/* message 3 from an access point with another passphrase, and one with a bit turned */
		check(lx_wpa_message(&wpa, h->m3_wrong, (unsigned)h->m3_wrong_length, reply, &length, &why) == LX_WPA_BAD &&
		    length == 0, "message 3 under another passphrase is refused", (int)n);
		memcpy(spoiled, h->m3, h->m3_length);
		spoiled[AT_DATA + 3] ^= 0x40;
		check(lx_wpa_message(&wpa, spoiled, (unsigned)h->m3_length, reply, &length, &why) == LX_WPA_BAD,
		    "message 3 with a bit turned is refused", (int)n);

		check(lx_wpa_message(&wpa, h->m3, (unsigned)h->m3_length, reply, &length, &why) == LX_WPA_KEYS &&
		    length == h->m4_length && memcmp(reply, h->m4, length) == 0, "message 4 is the reference's", (int)n);
		check(wpa.group_length == 16 && memcmp(wpa.group, h->group, 16) == 0 && wpa.group_index == h->group_index,
		    "the group key and its number are the reference's", (int)n);
		check(memcmp(wpa.group_counter, h->m3 + AT_RSC, 8) == 0, "the group key's packet number is message 3's", (int)n);

		/* message 3 once more: played again */
		check(lx_wpa_message(&wpa, h->m3, (unsigned)h->m3_length, reply, &length, &why) == LX_WPA_BAD,
		    "message 3 played again is refused", (int)n);

		/* a new group key later */
		check(lx_wpa_message(&wpa, h->g1, (unsigned)h->g1_length, reply, &length, &why) == LX_WPA_GROUP &&
		    length == h->g2_length && memcmp(reply, h->g2, length) == 0, "the answer to a new group key is the reference's",
		    (int)n);
		check(memcmp(wpa.group, h->group2, 16) == 0 && wpa.group_index == h->group_index % 3 + 1,
		    "the new group key and its number are the reference's", (int)n);

		/* a network that wants another kind of key management says so in the descriptor's version */
		memcpy(spoiled, h->m1, h->m1_length);
		spoiled[AT_INFO + 1] = (unsigned char)((spoiled[AT_INFO + 1] & ~7) | 3);
		check(lx_wpa_message(&wpa, spoiled, (unsigned)h->m1_length, reply, &length, &why) == LX_WPA_BAD && why[0] != 0,
		    "a key descriptor of version 3 is refused, with a reason", (int)n);

		/* things that are not key messages, and ones cut short */
		check(lx_wpa_message(&wpa, (const unsigned char *)"\x02\x00\x00\x00", 4, reply, &length, &why) == LX_WPA_NOTHING,
		    "an EAPOL frame that is not a key message is left alone", (int)n);
		check(lx_wpa_message(&wpa, h->m1, 60, reply, &length, &why) == LX_WPA_BAD, "a key message cut short is refused", (int)n);
	}
}

int
main(void)
{
	pieces();
	whole();
	printf("%u checks, %u failed\n", checks, failed);
	return failed != 0;
}
