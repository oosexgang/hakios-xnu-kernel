/*
 * minicrypto: message digests that the kernel's crypto interface needs but
 * that are not part of the SHA-256-only "built-in corecrypto" shipped in the
 * XNU sources: MD5, SHA-1, SHA-384 and SHA-512.
 *
 * Written from the public algorithm specifications (RFC 1321, FIPS 180-4).
 * The constant tables were generated from their mathematical definitions.
 * Plain portable C, no assembly; correctness is checked at boot against
 * reference vectors (see mc_digest_selftest).
 */
#include <corecrypto/ccdigest.h>
#include <corecrypto/ccdigest_priv.h>
#include <corecrypto/cc_priv.h>
#include <corecrypto/ccsha1.h>
#include <corecrypto/ccsha2.h>
#include <string.h>

#include "minicrypto.h"

/* from the kernel's built-in corecrypto subset (osfmk/corecrypto) */
extern void ccdigest_final_64be(const struct ccdigest_info *di, ccdigest_ctx_t ctx, unsigned char *digest);

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

/* ---------------------------------------------------------------- MD5 --- */

#define MD5_STATE_SIZE  16
#define MD5_BLOCK_SIZE  64
#define MD5_OUTPUT_SIZE 16

static const uint32_t md5_initial_state[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };

static const uint32_t md5_k[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
	0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
	0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
	0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
	0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
	0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
	0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
	0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
	0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t md5_s[64] = {
	7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
	5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void
md5_compress(ccdigest_state_t state, size_t nblocks, const void *in)
{
	uint32_t *s = ccdigest_u32(state);
	const uint8_t *buf = in;

	for (; nblocks > 0; nblocks--, buf += MD5_BLOCK_SIZE) {
		uint32_t m[16], a = s[0], b = s[1], c = s[2], d = s[3];

		for (int i = 0; i < 16; i++) {
			m[i] = cc_load32_le(buf + 4 * i);
		}
		for (int i = 0; i < 64; i++) {
			uint32_t f;
			int g;
			if (i < 16) {
				f = (b & c) | (~b & d);
				g = i;
			} else if (i < 32) {
				f = (d & b) | (~d & c);
				g = (5 * i + 1) & 15;
			} else if (i < 48) {
				f = b ^ c ^ d;
				g = (3 * i + 5) & 15;
			} else {
				f = c ^ (b | ~d);
				g = (7 * i) & 15;
			}
			f += a + md5_k[i] + m[g];
			a = d;
			d = c;
			c = b;
			b += ROL32(f, md5_s[i]);
		}
		s[0] += a;
		s[1] += b;
		s[2] += c;
		s[3] += d;
	}
}

/* Finalisation for digests with a 64-byte block and a little-endian bit count. */
static void
mc_final_64le(const struct ccdigest_info *di, ccdigest_ctx_t ctx, unsigned char *digest)
{
	if (ccdigest_num(di, ctx) >= di->block_size) {
		ccdigest_num(di, ctx) = 0;
	}
	ccdigest_di_decl(di, tmp);
	memcpy(tmp, ctx, ccdigest_di_size(di));

	ccdigest_nbits(di, tmp) += ccdigest_num(di, tmp) * 8;
	ccdigest_data(di, tmp)[ccdigest_num(di, tmp)++] = 0x80;
	if (ccdigest_num(di, tmp) > 64 - 8) {
		while (ccdigest_num(di, tmp) < 64) {
			ccdigest_data(di, tmp)[ccdigest_num(di, tmp)++] = 0;
		}
		di->compress(ccdigest_state(di, tmp), 1, ccdigest_data(di, tmp));
		ccdigest_num(di, tmp) = 0;
	}
	while (ccdigest_num(di, tmp) < 64 - 8) {
		ccdigest_data(di, tmp)[ccdigest_num(di, tmp)++] = 0;
	}
	uint64_t nbits = ccdigest_nbits(di, tmp);
	cc_store32_le((uint32_t)nbits, ccdigest_data(di, tmp) + 56);
	cc_store32_le((uint32_t)(nbits >> 32), ccdigest_data(di, tmp) + 60);
	di->compress(ccdigest_state(di, tmp), 1, ccdigest_data(di, tmp));

	for (unsigned int i = 0; i < di->output_size / 4; i++) {
		cc_store32_le(ccdigest_state_u32(di, tmp)[i], digest + 4 * i);
	}
	ccdigest_di_clear(di, tmp);
}

const struct ccdigest_info mc_md5_di = {
	.output_size = MD5_OUTPUT_SIZE,
	.state_size = MD5_STATE_SIZE,
	.block_size = MD5_BLOCK_SIZE,
	.oid_size = 10,
	.oid = CC_DIGEST_OID_MD5,
	.initial_state = md5_initial_state,
	.compress = md5_compress,
	.final = mc_final_64le,
};

/* -------------------------------------------------------------- SHA-1 --- */

static const uint32_t sha1_initial_state[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };

static void
sha1_compress(ccdigest_state_t state, size_t nblocks, const void *in)
{
	uint32_t *s = ccdigest_u32(state);
	const uint8_t *buf = in;

	for (; nblocks > 0; nblocks--, buf += CCSHA1_BLOCK_SIZE) {
		uint32_t w[80], a = s[0], b = s[1], c = s[2], d = s[3], e = s[4];

		for (int i = 0; i < 16; i++) {
			w[i] = cc_load32_be(buf + 4 * i);
		}
		for (int i = 16; i < 80; i++) {
			w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		}
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) {
				f = (b & c) | (~b & d);
				k = 0x5a827999;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ed9eba1;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8f1bbcdc;
			} else {
				f = b ^ c ^ d;
				k = 0xca62c1d6;
			}
			uint32_t t = ROL32(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = ROL32(b, 30);
			b = a;
			a = t;
		}
		s[0] += a;
		s[1] += b;
		s[2] += c;
		s[3] += d;
		s[4] += e;
	}
}

const struct ccdigest_info mc_sha1_di = {
	.output_size = CCSHA1_OUTPUT_SIZE,
	.state_size = CCSHA1_STATE_SIZE,
	.block_size = CCSHA1_BLOCK_SIZE,
	.oid_size = ccoid_sha1_len,
	.oid = CC_DIGEST_OID_SHA1,
	.initial_state = sha1_initial_state,
	.compress = sha1_compress,
	.final = ccdigest_final_64be,
};

/* ------------------------------------------------- SHA-512 / SHA-384 --- */

static const uint64_t sha512_initial_state[8] = {
	0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
	0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
	0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
	0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

static const uint64_t sha384_initial_state[8] = {
	0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL,
	0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
	0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
	0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL,
};

static const uint64_t sha512_k[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
	0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
	0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
	0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
	0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
	0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
	0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
	0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
	0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
	0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
	0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
	0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
	0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
	0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
	0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
	0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
	0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
	0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
	0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
	0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
	0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
	0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
	0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
	0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static void
sha512_compress(ccdigest_state_t state, size_t nblocks, const void *in)
{
	uint64_t *s = ccdigest_u64(state);
	const uint8_t *buf = in;

	for (; nblocks > 0; nblocks--, buf += CCSHA512_BLOCK_SIZE) {
		uint64_t w[80], v[8];

		for (int i = 0; i < 16; i++) {
			w[i] = cc_load64_be(buf + 8 * i);
		}
		for (int i = 16; i < 80; i++) {
			uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
			uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		memcpy(v, s, sizeof(v));
		for (int i = 0; i < 80; i++) {
			uint64_t S1 = ROR64(v[4], 14) ^ ROR64(v[4], 18) ^ ROR64(v[4], 41);
			uint64_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
			uint64_t t1 = v[7] + S1 + ch + sha512_k[i] + w[i];
			uint64_t S0 = ROR64(v[0], 28) ^ ROR64(v[0], 34) ^ ROR64(v[0], 39);
			uint64_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
			uint64_t t2 = S0 + maj;
			v[7] = v[6];
			v[6] = v[5];
			v[5] = v[4];
			v[4] = v[3] + t1;
			v[3] = v[2];
			v[2] = v[1];
			v[1] = v[0];
			v[0] = t1 + t2;
		}
		for (int i = 0; i < 8; i++) {
			s[i] += v[i];
		}
	}
}

/* Finalisation for 128-byte-block digests with a 128-bit big-endian bit count. */
static void
mc_final_128be(const struct ccdigest_info *di, ccdigest_ctx_t ctx, unsigned char *digest)
{
	if (ccdigest_num(di, ctx) >= di->block_size) {
		ccdigest_num(di, ctx) = 0;
	}
	ccdigest_di_decl(di, tmp);
	memcpy(tmp, ctx, ccdigest_di_size(di));

	ccdigest_nbits(di, tmp) += ccdigest_num(di, tmp) * 8;
	ccdigest_data(di, tmp)[ccdigest_num(di, tmp)++] = 0x80;
	if (ccdigest_num(di, tmp) > 128 - 16) {
		while (ccdigest_num(di, tmp) < 128) {
			ccdigest_data(di, tmp)[ccdigest_num(di, tmp)++] = 0;
		}
		di->compress(ccdigest_state(di, tmp), 1, ccdigest_data(di, tmp));
		ccdigest_num(di, tmp) = 0;
	}
	while (ccdigest_num(di, tmp) < 128 - 8) {
		ccdigest_data(di, tmp)[ccdigest_num(di, tmp)++] = 0;
	}
	cc_store64_be(ccdigest_nbits(di, tmp), ccdigest_data(di, tmp) + 128 - 8);
	di->compress(ccdigest_state(di, tmp), 1, ccdigest_data(di, tmp));

	for (unsigned int i = 0; i < di->output_size / 8; i++) {
		cc_store64_be(ccdigest_state_u64(di, tmp)[i], digest + 8 * i);
	}
	ccdigest_di_clear(di, tmp);
}

const struct ccdigest_info mc_sha512_di = {
	.output_size = CCSHA512_OUTPUT_SIZE,
	.state_size = CCSHA512_STATE_SIZE,
	.block_size = CCSHA512_BLOCK_SIZE,
	.oid_size = ccoid_sha512_len,
	.oid = CC_DIGEST_OID_SHA512,
	.initial_state = sha512_initial_state,
	.compress = sha512_compress,
	.final = mc_final_128be,
};

const struct ccdigest_info mc_sha384_di = {
	.output_size = CCSHA384_OUTPUT_SIZE,
	.state_size = CCSHA512_STATE_SIZE,
	.block_size = CCSHA512_BLOCK_SIZE,
	.oid_size = ccoid_sha384_len,
	.oid = CC_DIGEST_OID_SHA384,
	.initial_state = sha384_initial_state,
	.compress = sha512_compress,
	.final = mc_final_128be,
};

/* ---------------------------------------------------------- self test --- */

void
mc_digest(const struct ccdigest_info *di, size_t len, const void *data, void *digest)
{
	ccdigest_di_decl(di, ctx);
	ccdigest_init(di, ctx);
	ccdigest_update(di, ctx, len, data);
	di->final(di, ctx, digest);
	ccdigest_di_clear(di, ctx);
}

/* Reference digests of "abc" and of the 300-byte pattern msg[i] = 7*i + 3 (computed with OpenSSL). */
static const struct mc_vector {
	const struct ccdigest_info *di;
	const char *name;
	uint8_t abc[64];
	uint8_t pattern[64];
} mc_vectors[] = {
	{ &mc_md5_di, "md5", { 0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0, 0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72 }, { 0x3e, 0xa7, 0x4b, 0x65, 0x67, 0x3e, 0x13, 0x90, 0xb9, 0x0d, 0xa3, 0xe4, 0x62, 0xc0, 0x2f, 0x9f } },
	{ &mc_sha1_di, "sha1", { 0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d }, { 0x2b, 0x49, 0x8a, 0x21, 0x77, 0xb1, 0x81, 0xad, 0x0e, 0x5e, 0xdd, 0xbd, 0x5c, 0xb5, 0x8b, 0x8d, 0x52, 0xcf, 0x69, 0x41 } },
	{ &mc_sha384_di, "sha384", { 0xcb, 0x00, 0x75, 0x3f, 0x45, 0xa3, 0x5e, 0x8b, 0xb5, 0xa0, 0x3d, 0x69, 0x9a, 0xc6, 0x50, 0x07, 0x27, 0x2c, 0x32, 0xab, 0x0e, 0xde, 0xd1, 0x63, 0x1a, 0x8b, 0x60, 0x5a, 0x43, 0xff, 0x5b, 0xed, 0x80, 0x86, 0x07, 0x2b, 0xa1, 0xe7, 0xcc, 0x23, 0x58, 0xba, 0xec, 0xa1, 0x34, 0xc8, 0x25, 0xa7 }, { 0x06, 0x0e, 0xb8, 0xe1, 0xc0, 0x1c, 0xc6, 0xc6, 0xc8, 0xc0, 0x3f, 0xcb, 0x78, 0x98, 0x15, 0x5f, 0x2b, 0x83, 0x6f, 0xb9, 0x7c, 0x9d, 0x33, 0x1a, 0x7a, 0x29, 0x6a, 0x1a, 0x20, 0x9a, 0xac, 0x9d, 0x26, 0x1b, 0x2b, 0x26, 0x8c, 0x4b, 0xa3, 0xc7, 0x29, 0x9a, 0x3d, 0xd1, 0x2b, 0x5b, 0xf8, 0x1f } },
	{ &mc_sha512_di, "sha512", { 0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba, 0xcc, 0x41, 0x73, 0x49, 0xae, 0x20, 0x41, 0x31, 0x12, 0xe6, 0xfa, 0x4e, 0x89, 0xa9, 0x7e, 0xa2, 0x0a, 0x9e, 0xee, 0xe6, 0x4b, 0x55, 0xd3, 0x9a, 0x21, 0x92, 0x99, 0x2a, 0x27, 0x4f, 0xc1, 0xa8, 0x36, 0xba, 0x3c, 0x23, 0xa3, 0xfe, 0xeb, 0xbd, 0x45, 0x4d, 0x44, 0x23, 0x64, 0x3c, 0xe8, 0x0e, 0x2a, 0x9a, 0xc9, 0x4f, 0xa5, 0x4c, 0xa4, 0x9f }, { 0x46, 0xe5, 0x6a, 0xd3, 0x0d, 0xb9, 0xef, 0x50, 0xf8, 0xb6, 0x76, 0x2b, 0xa5, 0x58, 0x39, 0x73, 0x7f, 0x3f, 0xba, 0x34, 0xab, 0x47, 0x86, 0x3c, 0x9d, 0xaf, 0xf7, 0xb3, 0xf5, 0x8f, 0x97, 0xfe, 0x34, 0x65, 0xa5, 0x2d, 0xd3, 0x64, 0x56, 0x0d, 0xb4, 0x7f, 0x80, 0x29, 0x09, 0xce, 0xd4, 0x90, 0x93, 0x32, 0x26, 0x21, 0xea, 0x0a, 0xeb, 0xf8, 0xe0, 0x69, 0x6b, 0x85, 0xca, 0x8f, 0x81, 0xf0 } },
};

/* Returns the name of the first digest that fails, or NULL if all pass. */
const char *
mc_digest_selftest(void)
{
	uint8_t pattern[300], out[64];

	for (unsigned i = 0; i < sizeof(pattern); i++) {
		pattern[i] = (uint8_t)(i * 7 + 3);
	}
	for (unsigned v = 0; v < sizeof(mc_vectors) / sizeof(mc_vectors[0]); v++) {
		const struct mc_vector *vec = &mc_vectors[v];

		mc_digest(vec->di, 3, "abc", out);
		if (memcmp(out, vec->abc, vec->di->output_size) != 0) {
			return vec->name;
		}
		/* feed the pattern in uneven pieces to exercise the buffering */
		ccdigest_di_decl(vec->di, ctx);
		ccdigest_init(vec->di, ctx);
		ccdigest_update(vec->di, ctx, 1, pattern);
		ccdigest_update(vec->di, ctx, 130, pattern + 1);
		ccdigest_update(vec->di, ctx, 169, pattern + 131);
		vec->di->final(vec->di, ctx, out);
		if (memcmp(out, vec->pattern, vec->di->output_size) != 0) {
			return vec->name;
		}
	}
	return NULL;
}
