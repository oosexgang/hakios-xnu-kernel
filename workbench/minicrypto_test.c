/*
 * Host tests of minicrypto, the kernel's stand-in for corecrypto (crypto/mc_aes.c and mc_digests.c, at
 * osfmk/minicrypto in the kernel): workbench/run.sh builds them as the kernel has them, with the parts of
 * XNU's own corecrypto they rest on (digest and HMAC framing, cc_clear, cc_cmp_safe) from the kernel tree.
 *
 * The published vectors, through the descriptors the kernel calls (FIPS 197, SP 800-38A, RFC 1321,
 * FIPS 180-4, RFC 2202, RFC 4231); and a context that holds no key. Not every caller looks at what a key
 * schedule returned (ESP's does not), so a key of a length AES does not take must leave nothing usable
 * behind, and a context that was never keyed -- zeroed memory, or garbage -- must be refused by every
 * mode rather than run zero rounds and give the plaintext back as the ciphertext.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <corecrypto/ccdigest.h>
#include <corecrypto/cchmac.h>
#include <corecrypto/ccmode.h>

#include "minicrypto.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static size_t
unhex(const char *hex, uint8_t *out)
{
	size_t n = 0;

	for (; hex[0] != '\0' && hex[1] != '\0'; hex += 2) {
		unsigned int byte;

		sscanf(hex, "%2x", &byte);
		out[n++] = (uint8_t)byte;
	}
	return n;
}

/* got matches the hex string want (as many bytes as want has) */
static void
check_hex(const char *name, const uint8_t *got, const char *want)
{
	uint8_t expected[128];
	size_t n = unhex(want, expected);

	CHECK(memcmp(got, expected, n) == 0, "%s", name);
}

static bool
all_zero(const void *p, size_t n)
{
	const uint8_t *b = p;

	for (size_t i = 0; i < n; i++) {
		if (b[i] != 0) {
			return false;
		}
	}
	return true;
}

static void
test_selftests(void)
{
	const char *failed = mc_digest_selftest();

	CHECK(failed == NULL, "the digests' boot self-test: %s", failed);
	failed = mc_aes_selftest();
	CHECK(failed == NULL, "AES's boot self-test: %s", failed);
}

static void
test_aes_vectors(void)
{
	const struct ccmode_ecb *ee = &mc_aes_ecb_encrypt, *ed = &mc_aes_ecb_decrypt;
	const struct ccmode_cbc *ce = &mc_aes_cbc_encrypt, *cd = &mc_aes_cbc_decrypt;
	const struct ccmode_ctr *cr = &mc_aes_ctr_crypt;
	ccecb_ctx_decl(ee->size, e);
	ccecb_ctx_decl(ed->size, d);
	cccbc_ctx_decl(ce->size, c);
	cccbc_ctx_decl(cd->size, cdec);
	ccctr_ctx_decl(cr->size, r);
	uint8_t k[32], p[64], iv[16], o[64];
	/* FIPS 197 appendix C: AES-128, -192 and -256 on the same block, the key 00 01 02 ... */
	static const char *fips197[3] = {
		"69c4e0d86a7b0430d8cdb78070b4c55a", "dda97ca4864cdfe06eaf70a0ec0d7191", "8ea2b7ca516745bfeafc49904b496089",
	};

	unhex("00112233445566778899aabbccddeeff", p);
	unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", k);
	for (int i = 0; i < 3; i++) {
		size_t key_len = 16 + 8 * (size_t)i;

		CHECK(ee->init(ee, e, key_len, k) == 0, "a %zu-byte key", key_len);
		CHECK(ee->ecb(e, 1, p, o) == 0, "encrypt");
		check_hex("FIPS 197 C encrypt", o, fips197[i]);
		CHECK(ed->init(ed, d, key_len, k) == 0, "a %zu-byte key", key_len);
		CHECK(ed->ecb(d, 1, o, o) == 0, "decrypt");
		check_hex("FIPS 197 C decrypt (in place)", o, "00112233445566778899aabbccddeeff");
	}

	/* SP 800-38A F.2.1 / F.2.2: CBC-AES128, two calls to see the IV carried over */
	unhex("2b7e151628aed2a6abf7158809cf4f3c", k);
	unhex("000102030405060708090a0b0c0d0e0f", iv);
	unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", p);
	CHECK(ce->init(ce, c, 16, k) == 0, "CBC key");
	CHECK(ce->cbc(c, (cccbc_iv *)iv, 1, p, o) == 0 && ce->cbc(c, (cccbc_iv *)iv, 1, p + 16, o + 16) == 0, "CBC encrypt");
	check_hex("SP 800-38A F.2.1", o, "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2");
	unhex("000102030405060708090a0b0c0d0e0f", iv);
	CHECK(cd->init(cd, cdec, 16, k) == 0, "CBC key");
	CHECK(cd->cbc(cdec, (cccbc_iv *)iv, 2, o, o) == 0, "CBC decrypt");
	check_hex("SP 800-38A F.2.2 (in place)", o, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");

	/* SP 800-38A F.5.1 and F.5.5: CTR-AES128 and CTR-AES256, the 128-bit counter f0f1...ff */
	unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv);
	CHECK(cr->init(cr, r, 16, k, iv) == 0, "CTR key");
	CHECK(cr->ctr(r, 5, p, o) == 0 && cr->ctr(r, 27, p + 5, o + 5) == 0, "CTR in uneven pieces");
	check_hex("SP 800-38A F.5.1", o, "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff");
	unhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", k);
	CHECK(cr->init(cr, r, 32, k, iv) == 0, "CTR key");
	CHECK(cr->ctr(r, 16, p, o) == 0, "CTR");
	check_hex("SP 800-38A F.5.5", o, "601ec313775789a5b7a7f504bbf3d228");
}

static void
test_digest_vectors(void)
{
	static const char m448[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	static const char m896[] = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
	static const char m80[] = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
	static const char jefe[] = "what do ya want for nothing?";
	static const char tc6[] = "Test Using Larger Than Block-Size Key - Hash Key First";
	uint8_t o[64], big_key[131];

	mc_digest(&mc_md5_di, strlen(m80), m80, o);
	check_hex("RFC 1321 MD5", o, "57edf4a22be3c955ac49da2e2107b67a");
	mc_digest(&mc_sha1_di, strlen(m448), m448, o);
	check_hex("FIPS 180 SHA-1 448 bits", o, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
	mc_digest(&mc_sha384_di, strlen(m896), m896, o);
	check_hex("FIPS 180 SHA-384 896 bits", o, "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039");
	mc_digest(&mc_sha512_di, strlen(m896), m896, o);
	check_hex("FIPS 180 SHA-512 896 bits", o,
	    "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");

	cchmac(&mc_md5_di, 4, "Jefe", strlen(jefe), jefe, o);
	check_hex("RFC 2202 HMAC-MD5 2", o, "750c783e6ab0b503eaa86e310a5db738");
	cchmac(&mc_sha1_di, 4, "Jefe", strlen(jefe), jefe, o);
	check_hex("RFC 2202 HMAC-SHA-1 2", o, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
	cchmac(&mc_sha384_di, 4, "Jefe", strlen(jefe), jefe, o);
	check_hex("RFC 4231 HMAC-SHA-384 2", o, "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e8e2240ca5e69e2c78b3239ecfab21649");
	cchmac(&mc_sha512_di, 4, "Jefe", strlen(jefe), jefe, o);
	check_hex("RFC 4231 HMAC-SHA-512 2", o,
	    "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737");
	memset(big_key, 0xaa, sizeof(big_key));
	cchmac(&mc_sha384_di, sizeof(big_key), big_key, strlen(tc6), tc6, o);
	check_hex("RFC 4231 HMAC-SHA-384 6", o, "4ece084485813e9088d2c63a041bc5b44f9ef1012a2b588f3cd11f05033ac4c60c2ef6ab4030fe8296248df163f44952");
	cchmac(&mc_sha512_di, sizeof(big_key), big_key, strlen(tc6), tc6, o);
	check_hex("RFC 4231 HMAC-SHA-512 6", o,
	    "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f3526b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598");
}

/* ------------------------------------------------- a context with no key --- */

static const uint8_t good_key[32] = {
	0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
	0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7, 0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4,
};
static const uint8_t plain[32] = "attack at dawn, secret payload!";

/* key lengths AES does not take: none, a DES key, 160 bits (within ESP's 128..256), one past 256 */
static const size_t bad_lengths[] = { 0, 8, 20, 33 };

/*
 * What a mode does with ctx: it must fail and must not give back the plaintext; what it writes
 * instead is zeros, also over the plaintext when it works in place.
 */
static void
check_refused(const char *mode, const char *how, int rc, const uint8_t *out, size_t n)
{
	CHECK(rc != 0, "%s on %s: returned 0", mode, how);
	CHECK(memcmp(out, plain, n) != 0, "%s on %s: the plaintext came out as the ciphertext", mode, how);
	CHECK(all_zero(out, n), "%s on %s: the output is not zeroed", mode, how);
}

static int
run_ecb(const struct ccmode_ecb *mode, const void *ctx, uint8_t *out)
{
	memcpy(out, plain, sizeof(plain));
	return mode->ecb(ctx, 2, out, out);
}

static int
run_cbc(const struct ccmode_cbc *mode, const void *ctx, uint8_t *out)
{
	uint8_t iv[16] = { 0 };

	memcpy(out, plain, sizeof(plain));
	return mode->cbc(ctx, (cccbc_iv *)iv, 2, out, out);
}

static int
run_ctr(const struct ccmode_ctr *mode, void *ctx, uint8_t *out)
{
	memcpy(out, plain, sizeof(plain));
	return mode->ctr(ctx, sizeof(plain), out, out);
}

static void
test_bad_key_ecb(const char *name, const struct ccmode_ecb *mode)
{
	uint8_t *ctx = malloc(mode->size), out[32];
	char how[64];

	for (size_t i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); i++) {
		snprintf(how, sizeof(how), "a %zu-byte key after a good one", bad_lengths[i]);
		CHECK(mode->init(mode, (ccecb_ctx *)ctx, 32, good_key) == 0, "%s: a good key", name);
		CHECK(mode->init(mode, (ccecb_ctx *)ctx, bad_lengths[i], good_key) != 0, "%s: %s accepted", name, how);
		CHECK(all_zero(ctx, mode->size), "%s: %s: the context is not cleared", name, how);
		check_refused(name, how, run_ecb(mode, ctx, out), out, sizeof(out));
	}
	memset(ctx, 0, mode->size);
	check_refused(name, "a zeroed context", run_ecb(mode, ctx, out), out, sizeof(out));
	fflush(stdout);
	/* (with no check of the round count this reads far past the key) */
	memset(ctx, 0xa5, mode->size);
	check_refused(name, "a context of garbage", run_ecb(mode, ctx, out), out, sizeof(out));
	free(ctx);
}

static void
test_bad_key_cbc(const char *name, const struct ccmode_cbc *mode)
{
	uint8_t *ctx = malloc(mode->size), out[32];
	char how[64];

	for (size_t i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); i++) {
		snprintf(how, sizeof(how), "a %zu-byte key after a good one", bad_lengths[i]);
		CHECK(mode->init(mode, (cccbc_ctx *)ctx, 32, good_key) == 0, "%s: a good key", name);
		CHECK(mode->init(mode, (cccbc_ctx *)ctx, bad_lengths[i], good_key) != 0, "%s: %s accepted", name, how);
		CHECK(all_zero(ctx, mode->size), "%s: %s: the context is not cleared", name, how);
		check_refused(name, how, run_cbc(mode, ctx, out), out, sizeof(out));
	}
	memset(ctx, 0, mode->size);
	check_refused(name, "a zeroed context", run_cbc(mode, ctx, out), out, sizeof(out));
	fflush(stdout);
	memset(ctx, 0xa5, mode->size);
	check_refused(name, "a context of garbage", run_cbc(mode, ctx, out), out, sizeof(out));
	free(ctx);
}

static void
test_bad_key_ctr(void)
{
	const struct ccmode_ctr *mode = &mc_aes_ctr_crypt;
	uint8_t *ctx = malloc(mode->size), out[32], iv[16] = { 0 };
	char how[64];

	for (size_t i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); i++) {
		snprintf(how, sizeof(how), "a %zu-byte key after a good one", bad_lengths[i]);
		CHECK(mode->init(mode, (ccctr_ctx *)ctx, 32, good_key, iv) == 0, "ctr: a good key");
		CHECK(run_ctr(mode, ctx, out) == 0, "ctr: a good key encrypts");    /* leaves keystream behind */
		CHECK(mode->init(mode, (ccctr_ctx *)ctx, bad_lengths[i], good_key, iv) != 0, "ctr: %s accepted", how);
		CHECK(all_zero(ctx, mode->size), "ctr: %s: the context (key, counter, keystream) is not cleared", how);
		check_refused("ctr", how, run_ctr(mode, ctx, out), out, sizeof(out));
	}
	memset(ctx, 0, mode->size);
	check_refused("ctr", "a zeroed context", run_ctr(mode, ctx, out), out, sizeof(out));
	free(ctx);
}

static void
test_bad_key(void)
{
	test_bad_key_ecb("ecb encrypt", &mc_aes_ecb_encrypt);
	test_bad_key_ecb("ecb decrypt", &mc_aes_ecb_decrypt);
	test_bad_key_cbc("cbc encrypt", &mc_aes_cbc_encrypt);
	test_bad_key_cbc("cbc decrypt", &mc_aes_cbc_decrypt);
	test_bad_key_ctr();
}

int
main(void)
{
	test_selftests();
	test_aes_vectors();
	test_digest_vectors();
	fflush(stdout);
	test_bad_key();
	printf("minicrypto_test: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
