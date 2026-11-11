/*
 * minicrypto provider: registers a crypto backend with the kernel in place
 * of Apple's corecrypto kext (com.apple.kec.corecrypto), which is not part
 * of the open-source release and cannot be loaded into this static kernel.
 *
 * Provided:
 *   - digests: MD5, SHA-1, SHA-256 (the kernel's built-in), SHA-384, SHA-512
 *   - HMAC over those digests
 *   - the kernel PRNG, an HMAC-DRBG (SHA-256) seeded from the boot seed and
 *     reseeded from the kernel entropy collector
 *   - the small non-cryptographic generator the VM layer asks for
 *
 *   - AES in ECB, CBC and CTR modes (mc_aes.c; the TCP stack needs it at boot)
 *
 * NOT provided (function pointers left NULL; using them will fault): AES-XTS,
 * AES-GCM, DES/3DES, ChaCha20-Poly1305, RSA. Encrypted volumes, IPsec and
 * signature checks need those; the boot path to a ramdisk shell does not.
 */
#include <mach/mach_types.h>
#include <kern/debug.h>
#include <kern/startup.h>
#include <kern/simple_lock.h>
#include <machine/machine_routines.h>
#include <libkern/crypto/register_crypto.h>
#include <libkern/crypto/rand.h>
#include <corecrypto/cc_priv.h>
#include <corecrypto/ccdigest.h>
#include <corecrypto/ccdrbg.h>
#include <corecrypto/cchmac.h>
#include <corecrypto/cckprng.h>
#include <corecrypto/ccrng.h>
#include <corecrypto/ccsha2.h>
#include <prng/random.h>
#include <string.h>

#include "minicrypto.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

extern void read_random(void *buffer, u_int numBytes);
extern int printf(const char *format, ...) __printflike(1, 2);

/* ------------------------------------------------------- kernel PRNG --- */

static struct {
	struct ccdrbg_info info;
	struct ccdrbg_nisthmac_custom custom;
	uint8_t state[512];
	cckprng_getentropy getentropy;
	void *getentropy_arg;
	bool ready;
	simple_lock_data_t lock;
} mc_prng;

static boolean_t
mc_prng_lock(void)
{
	boolean_t intr = ml_set_interrupts_enabled(FALSE);
	simple_lock(&mc_prng.lock, LCK_GRP_NULL);
	return intr;
}

static void
mc_prng_unlock(boolean_t intr)
{
	simple_unlock(&mc_prng.lock);
	ml_set_interrupts_enabled(intr);
}

static void
mc_prng_init_with_getentropy(struct cckprng_ctx *ctx __unused, unsigned max_ngens __unused, size_t seed_nbytes,
    const void *seed, size_t nonce_nbytes, const void *nonce, cckprng_getentropy getentropy, void *getentropy_arg)
{
	static const char ps[] = "xnu minicrypto kernel prng";

	simple_lock_init(&mc_prng.lock, 0);
	mc_prng.custom.di = &ccsha256_ltc_di;
	mc_prng.custom.strictFIPS = 0;
	ccdrbg_factory_nisthmac(&mc_prng.info, &mc_prng.custom);
	if (mc_prng.info.size > sizeof(mc_prng.state)) {
		panic("minicrypto: DRBG state does not fit (%zu)", mc_prng.info.size);
	}
	int rc = ccdrbg_init(&mc_prng.info, (struct ccdrbg_state *)mc_prng.state, seed_nbytes, seed, nonce_nbytes, nonce,
	    sizeof(ps) - 1, ps);
	if (rc != CCDRBG_STATUS_OK) {
		panic("minicrypto: ccdrbg_init failed (%d)", rc);
	}
	mc_prng.getentropy = getentropy;
	mc_prng.getentropy_arg = getentropy_arg;
	mc_prng.ready = true;
}

static void
mc_prng_init(struct cckprng_ctx *ctx, size_t seed_nbytes, const void *seed, size_t nonce_nbytes, const void *nonce,
    cckprng_getentropy getentropy, void *getentropy_arg)
{
	mc_prng_init_with_getentropy(ctx, 1, seed_nbytes, seed, nonce_nbytes, nonce, getentropy, getentropy_arg);
}

static void
mc_prng_initgen(struct cckprng_ctx *ctx __unused, unsigned gen_idx __unused)
{
	/* one generator shared by all CPUs */
}

/* Mix caller-supplied bytes into the generator (HMAC-DRBG "additional input"). */
static void
mc_prng_reseed(struct cckprng_ctx *ctx __unused, size_t nbytes, const void *seed)
{
	uint8_t discard[32];

	if (!mc_prng.ready || nbytes == 0) {
		return;
	}
	boolean_t intr = mc_prng_lock();
	(void)ccdrbg_generate(&mc_prng.info, (struct ccdrbg_state *)mc_prng.state, sizeof(discard), discard, nbytes, seed);
	mc_prng_unlock(intr);
	cc_clear(sizeof(discard), discard);
}

/* Pull whatever the kernel entropy collector has accumulated and mix it in. */
static void
mc_prng_refresh(struct cckprng_ctx *ctx)
{
	uint8_t entropy[CCKPRNG_ENTROPY_SIZE];
	size_t nbytes = sizeof(entropy);

	if (!mc_prng.ready || mc_prng.getentropy == NULL) {
		return;
	}
	if (mc_prng.getentropy(&nbytes, entropy, mc_prng.getentropy_arg) > 0 && nbytes > 0) {
		mc_prng_reseed(ctx, nbytes, entropy);
	}
	cc_clear(sizeof(entropy), entropy);
}

static void
mc_prng_generate(struct cckprng_ctx *ctx __unused, unsigned gen_idx __unused, size_t nbytes, void *out)
{
	uint8_t *p = out;

	while (nbytes > 0) {
		size_t n = MIN(nbytes, (size_t)1024);
		boolean_t intr = mc_prng_lock();
		int rc = ccdrbg_generate(&mc_prng.info, (struct ccdrbg_state *)mc_prng.state, n, p, 0, NULL);
		mc_prng_unlock(intr);
		if (rc != CCDRBG_STATUS_OK) {
			panic("minicrypto: ccdrbg_generate failed (%d)", rc);
		}
		p += n;
		nbytes -= n;
	}
}

static const struct cckprng_funcs mc_prng_funcs = {
	.init = mc_prng_init,
	.initgen = mc_prng_initgen,
	.reseed = mc_prng_reseed,
	.refresh = mc_prng_refresh,
	.generate = mc_prng_generate,
	.init_with_getentropy = mc_prng_init_with_getentropy,
};

/* ccrng handle: random bytes straight from the kernel PRNG. */
static int
mc_rng_generate(struct ccrng_state *rng __unused, size_t outlen, void *out)
{
	read_random(out, (u_int)outlen);
	return 0;
}

static struct ccrng_state mc_rng_state = { .generate = mc_rng_generate };

static struct ccrng_state *
mc_ccrng(int *error)
{
	if (error != NULL) {
		*error = 0;
	}
	return &mc_rng_state;
}

/* ------------------------------------- lock-free generator for kmem --- */
/* xoshiro256**: fast, no locks, explicitly not cryptographic (see rand.h). */

struct mc_random_ctx {
	uint64_t s[4];
};

static inline uint64_t
mc_rotl(uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

static uint64_t
mc_random_next(struct mc_random_ctx *r)
{
	uint64_t result = mc_rotl(r->s[1] * 5, 7) * 9;
	uint64_t t = r->s[1] << 17;

	r->s[2] ^= r->s[0];
	r->s[3] ^= r->s[1];
	r->s[1] ^= r->s[2];
	r->s[0] ^= r->s[3];
	r->s[2] ^= t;
	r->s[3] = mc_rotl(r->s[3], 45);
	return result;
}

static size_t
mc_random_kmem_ctx_size(void)
{
	return sizeof(struct mc_random_ctx);
}

static void
mc_random_kmem_init(crypto_random_ctx_t ctx)
{
	struct mc_random_ctx *r = ctx;

	read_random(r->s, sizeof(r->s));
	r->s[0] |= 1; /* the all-zero state is invalid */
}

static void
mc_random_generate(crypto_random_ctx_t ctx, void *random, size_t random_size)
{
	uint8_t *p = random;

	while (random_size > 0) {
		uint64_t v = mc_random_next(ctx);
		size_t n = MIN(random_size, sizeof(v));
		memcpy(p, &v, n);
		p += n;
		random_size -= n;
	}
}

static void
mc_random_uniform(crypto_random_ctx_t ctx, uint64_t bound, uint64_t *random)
{
	if (bound < 2) {
		*random = 0;
		return;
	}
	/* rejection sampling keeps the distribution flat */
	uint64_t limit = UINT64_MAX - (UINT64_MAX % bound);
	uint64_t v;
	do {
		v = mc_random_next(ctx);
	} while (v >= limit);
	*random = v % bound;
}

/* ---------------------------------------- generic digest / HMAC API --- */

static const struct ccdigest_info *
mc_di(crypto_digest_alg_t alg)
{
	switch (alg) {
	case CRYPTO_DIGEST_ALG_MD5:
		return &mc_md5_di;
	case CRYPTO_DIGEST_ALG_SHA1:
		return &mc_sha1_di;
	case CRYPTO_DIGEST_ALG_SHA256:
		return ccsha256_di();
	case CRYPTO_DIGEST_ALG_SHA384:
		return &mc_sha384_di;
	case CRYPTO_DIGEST_ALG_SHA512:
		return &mc_sha512_di;
	default:
		panic("minicrypto: unsupported digest algorithm %d", (int)alg);
	}
}

static void
mc_ccdigest_final(const struct ccdigest_info *di, ccdigest_ctx_t ctx, void *digest)
{
	di->final(di, ctx, digest);
}

static void
mc_ccdigest(const struct ccdigest_info *di, unsigned long len, const void *data, void *digest)
{
	mc_digest(di, len, data, digest);
}

static size_t
mc_digest_ctx_size(crypto_digest_alg_t alg)
{
	return ccdigest_di_size(mc_di(alg));
}

static void
mc_digest_init(crypto_digest_alg_t alg, void *ctx, size_t ctx_size __unused)
{
	ccdigest_init(mc_di(alg), ctx);
}

static void
mc_digest_update(crypto_digest_alg_t alg, void *ctx, size_t ctx_size __unused, const void *data, size_t data_size)
{
	ccdigest_update(mc_di(alg), ctx, data_size, data);
}

static void
mc_digest_final(crypto_digest_alg_t alg, void *ctx, size_t ctx_size __unused, void *digest, size_t digest_size)
{
	const struct ccdigest_info *di = mc_di(alg);
	uint8_t out[64];

	di->final(di, ctx, out);
	memcpy(digest, out, MIN(digest_size, di->output_size));
}

static void
mc_digest_oneshot(crypto_digest_alg_t alg, const void *data, size_t data_size, void *digest, size_t digest_size)
{
	const struct ccdigest_info *di = mc_di(alg);
	uint8_t out[64];

	mc_digest(di, data_size, data, out);
	memcpy(digest, out, MIN(digest_size, di->output_size));
}

static size_t
mc_hmac_ctx_size(crypto_digest_alg_t alg)
{
	return cchmac_di_size(mc_di(alg));
}

static void
mc_hmac_init(crypto_digest_alg_t alg, void *ctx, size_t ctx_size __unused, const void *key, size_t key_size)
{
	cchmac_init(mc_di(alg), ctx, key_size, key);
}

static void
mc_hmac_update(crypto_digest_alg_t alg, void *ctx, size_t ctx_size __unused, const void *data, size_t data_size)
{
	cchmac_update(mc_di(alg), ctx, data_size, data);
}

static void
mc_hmac_final_generate(crypto_digest_alg_t alg, void *ctx, size_t ctx_size __unused, void *tag, size_t tag_size)
{
	const struct ccdigest_info *di = mc_di(alg);
	uint8_t out[64];

	cchmac_final(di, ctx, out);
	memcpy(tag, out, MIN(tag_size, di->output_size));
	cc_clear(sizeof(out), out);
}

static bool
mc_hmac_final_verify(crypto_digest_alg_t alg, void *ctx, size_t ctx_size __unused, const void *tag, size_t tag_size)
{
	const struct ccdigest_info *di = mc_di(alg);
	uint8_t out[64];

	if (tag_size == 0 || tag_size > di->output_size) {
		return false;
	}
	cchmac_final(di, ctx, out);
	bool ok = (cc_cmp_safe(tag_size, out, tag) == 0);
	cc_clear(sizeof(out), out);
	return ok;
}

static void
mc_hmac_generate(crypto_digest_alg_t alg, const void *key, size_t key_size, const void *data, size_t data_size,
    void *tag, size_t tag_size)
{
	const struct ccdigest_info *di = mc_di(alg);
	uint8_t out[64];

	cchmac(di, key_size, key, data_size, data, out);
	memcpy(tag, out, MIN(tag_size, di->output_size));
	cc_clear(sizeof(out), out);
}

static bool
mc_hmac_verify(crypto_digest_alg_t alg, const void *key, size_t key_size, const void *data, size_t data_size,
    const void *tag, size_t tag_size)
{
	const struct ccdigest_info *di = mc_di(alg);
	uint8_t out[64];

	if (tag_size == 0 || tag_size > di->output_size) {
		return false;
	}
	cchmac(di, key_size, key, data_size, data, out);
	bool ok = (cc_cmp_safe(tag_size, out, tag) == 0);
	cc_clear(sizeof(out), out);
	return ok;
}

/* ------------------------------------------------------ registration --- */

static struct crypto_functions mc_funcs = {
	.ccdigest_init_fn = ccdigest_init,
	.ccdigest_update_fn = ccdigest_update,
	.ccdigest_final_fn = mc_ccdigest_final,
	.ccdigest_fn = mc_ccdigest,
	.ccmd5_di = &mc_md5_di,
	.ccsha1_di = &mc_sha1_di,
	.ccsha384_di = &mc_sha384_di,
	.ccsha512_di = &mc_sha512_di,

	.cchmac_init_fn = cchmac_init,
	.cchmac_update_fn = cchmac_update,
	.cchmac_final_fn = cchmac_final,
	.cchmac_fn = cchmac,

	.ccaes_ecb_encrypt = &mc_aes_ecb_encrypt,
	.ccaes_ecb_decrypt = &mc_aes_ecb_decrypt,
	.ccaes_cbc_encrypt = &mc_aes_cbc_encrypt,
	.ccaes_cbc_decrypt = &mc_aes_cbc_decrypt,
	.ccaes_ctr_crypt = &mc_aes_ctr_crypt,

	.ccrng_fn = mc_ccrng,

	.random_generate_fn = mc_random_generate,
	.random_uniform_fn = mc_random_uniform,
	.random_kmem_ctx_size_fn = mc_random_kmem_ctx_size,
	.random_kmem_init_fn = mc_random_kmem_init,

	.digest_ctx_size_fn = mc_digest_ctx_size,
	.digest_init_fn = mc_digest_init,
	.digest_update_fn = mc_digest_update,
	.digest_final_fn = mc_digest_final,
	.digest_fn = mc_digest_oneshot,

	.hmac_ctx_size_fn = mc_hmac_ctx_size,
	.hmac_init_fn = mc_hmac_init,
	.hmac_update_fn = mc_hmac_update,
	.hmac_final_generate_fn = mc_hmac_final_generate,
	.hmac_final_verify_fn = mc_hmac_final_verify,
	.hmac_generate_fn = mc_hmac_generate,
	.hmac_verify_fn = mc_hmac_verify,
};

/*
 * Runs where the corecrypto kext would normally have been started: before
 * the first consumer in the EARLY_BOOT stage (kmem's random generator).
 */
__startup_func
static void
minicrypto_init(void)
{
	const char *failed = mc_digest_selftest();
	if (failed == NULL) {
		failed = mc_aes_selftest();
	}
	if (failed != NULL) {
		panic("minicrypto: %s self-test failed", failed);
	}

	mc_funcs.ccsha256_di = ccsha256_di();
	if (register_crypto_functions(&mc_funcs) != 0) {
		return; /* a real provider got there first */
	}
	/* The PRNG context is ours; the kernel only hands the pointer back to us. */
	register_and_init_prng((struct cckprng_ctx *)(void *)&mc_prng, &mc_prng_funcs);
	printf("minicrypto: registered (md5, sha1, sha2, hmac, aes-ecb/cbc/ctr, hmac-drbg prng); no xts/gcm, no RSA\n");
}
STARTUP(EARLY_BOOT, STARTUP_RANK_FIRST, minicrypto_init);
